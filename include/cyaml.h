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

/* Everything that this header declares from here to its end is part of the
 * public Application Binary Interface (ABI) of libccollections. The shared
 * library exports all of it. The library is built with
 * -fvisibility=hidden. A function or object that is not inside one of these
 * blocks stays internal to the library. Its name is absent from the dynamic
 * symbol table of the library. The application that links against the
 * library cannot interpose it. A symbol of the same name in that
 * application cannot collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cyaml.h
 * @brief YAML 1.2 parser, serializer, and mutable in-memory DOM.
 *
 * The DOM uses the same design as the cjson module. Every node is an opaque
 * struct on the heap. A cvector holds the elements of a list. A chashmap
 * holds the entries of a dictionary.
 *
 * ### Supported YAML features
 *
 * - Block dictionaries and lists, and flow dictionaries and lists.
 * - All scalar styles: plain, single-quoted, double-quoted, literal (|) and
 *   folded (>). All chomping modes (clip, strip, keep) are available. A
 *   plain, single-quoted or double-quoted scalar can cover more than one
 *   physical line. The parser folds such a scalar with the same rules as a
 *   folded block scalar. One line break folds to a space. A run of blank
 *   lines folds to that many newlines instead. An implicit mapping key must
 *   still fit on one physical line. Only an implicit *value* or an explicit
 *   key (? key) can cover more than one line.
 * - Anchors (&name) and aliases (*name).
 * - Implicit core-schema typing for null, bool, integer (decimal, octal or
 *   hex) and float. A float includes .inf and .nan.
 * - Non-scalar dictionary keys. The dictionaries of this DOM always map
 *   char* -> cyaml. This is why the parser canonicalizes a sequence,
 *   mapping or flow collection that is a key directly into compact
 *   flow-style text. It canonicalizes the entries of a mapping in sorted
 *   order at every nesting level. The sort is lexicographic by key, and the
 *   insertion order has no effect on it. Two non-scalar keys with the same
 *   structure therefore always canonicalize to the same text. They collide
 *   correctly as one key, whatever order the caller built them in. The
 *   element order of a sequence is always significant, and the parser never
 *   reorders it.
 * - Scalar dictionary keys. The parser resolves the core-schema type of a
 *   plain key and stores its canonical text. A null key gives "null", a
 *   bool key "true" or "false", and an integer key its decimal text, so
 *   "0x10" gives "16". A float key gives the shortest decimal text that
 *   reads back as the same double, so "3.10" gives "3.1" and "1e300" gives
 *   "1e+300". ".inf", "-.inf" and ".nan" keep those spellings, both zeros
 *   give "0", and an integral float carries no fractional part, so the keys
 *   1.0 and 1 are one key. "yes", "no", "on" and "off" are strings under
 *   YAML 1.2 and keep their own text. A quoted key with no tag keeps its
 *   text exactly. A tag on a key types it first, as it types a value.
 *   The serializers write a key that is exactly the canonical text of an
 *   integer or a float with no quotes, and quote every other key that a
 *   parse would read as another type or another text.
 * - A flow collection across lines. Every continuation line must be
 *   indented more than the block mapping key or the "- " that holds the
 *   collection. Indentation counts spaces only; tabs after those spaces are
 *   separation, so JSON indented with tabs parses at the top level. A line
 *   that starts with the closing ']' or '}' may sit exactly at that
 *   indentation, after spaces only, which closes the collection under the
 *   line that opened it.
 * - Multi-document streams that "---" delimits. The first "---" is optional
 *   for an input with one document. For more than one document, the parser
 *   gives a CYAML_LIST with one element for each document root. For one
 *   document, it gives the root node of that document directly. A later
 *   document can omit its own "---" only when the document just before it
 *   ended with an explicit "...". A document that carries its own %YAML
 *   directive always needs an explicit "..." before it. This is true even
 *   when that document gives its own "---".
 * - The %YAML directive. The parser checks it against the MAJOR.MINOR
 *   grammar, and one document can hold at most one of them. The version
 *   number has no effect on the parse. The parser reads every input as
 *   YAML 1.2, whatever version the input declares.
 * - A tab character, as YAML 1.2 defines it. Indentation is spaces only,
 *   and a tab is separation whitespace wherever the grammar allows
 *   separation: after a "-", "?" or ":" indicator ("key:\tvalue"), after
 *   the spaces that indent a line, inside a flow collection, after "---"
 *   or "...", between the parts of a directive, and before a comment. A tab
 *   is refused where it would decide a block-structural column: in front
 *   of a block mapping key or a block sequence "-" on its line ("\ta: 1",
 *   "key:\t- a", "-\tb: c"), and as the indentation of a block scalar.
 *   Inside a scalar a tab is content. A line indented by tabs alone is
 *   therefore indented by zero spaces.
 * - A UTF-8 byte order mark in the prefix of a document: at the start of
 *   the stream, and at the start of a line before a later document (after
 *   a "..." or before a "---"), or at the end of the stream. A prefix may
 *   hold several marks, with comment lines between them. The mark is not
 *   content, and a comment may follow it directly. YAML 1.2 section 5.2
 *   forbids a mark inside a document, so a mark at the start of a line
 *   anywhere else is a parse error: a concatenation of a file that ends
 *   without a "..." and a file that starts with a mark needs a "---" after
 *   the mark. A mark in the middle of a line is ordinary content.
 * - **Tags** (!!str, !!binary, a custom !foo, a shorthand that %TAG
 *   resolves, or a verbatim !<...> tag). The parser resolves them, and
 *   cyaml_node_tag() reads them back. The "Tags" section below gives the
 *   full rules.
 * - **%TAG directive** scoping for a shorthand prefix. The parser tracks
 *   this for each document. A %TAG directive in one document never changes
 *   another document. A shorthand tag with an undefined handle is a parse
 *   error.
 * - **Merge keys** (<<:). A "<<:" entry of a mapping expands the mappings
 *   that it names into that mapping in place. The entry does this when its
 *   key reads exactly "<<" and is a plain, untagged scalar, or carries the
 *   core merge tag (!!merge). The "Merge keys" section below gives the full
 *   rules.
 *
 * ### Unsupported / explicitly out-of-scope
 *
 * - Null bytes inside strings. The scalar value of a node is a plain
 *   null-terminated C string with no separate length field. A null byte
 *   inside such a string can never be represented. Do not put a null byte
 *   into a string value. The parser rejects every double-quoted escape that
 *   decodes to codepoint zero ("\0", "\x00", "\u0000", "\U00000000") as a
 *   parse error. It does not truncate the value silently. Encode as base64
 *   any data that can hold a null byte. For the same reason, !!binary gets
 *   no special decode behavior. The library stores its base64 text as an
 *   ordinary string with the tag "tag:yaml.org,2002:binary". It never
 *   decodes that text into raw bytes.
 * - Any input that is not UTF-8 text made only of the printable characters
 *   of YAML 1.2 section 5.1 (c-printable): TAB, LF, CR, 0x20 to 0x7E, NEL
 *   (U+0085), U+00A0 to U+D7FF, U+E000 to U+FFFD and U+10000 to U+10FFFF.
 *   The parser checks every byte of the stream before it parses anything:
 *   scalars of every style, keys, comments, tags, anchors, directives and
 *   the space between tokens. It refuses the whole document at the first
 *   byte that breaks the rule. That is a byte sequence that is not
 *   well-formed UTF-8 (a truncated sequence, a continuation byte with no
 *   lead byte, an overlong form, an encoded surrogate, a code point above
 *   U+10FFFF, a byte from 0xF5 to 0xFF), a C0 control other than TAB, LF
 *   and CR, DEL, a C1 control (U+0080 to U+009F) other than NEL, or one of
 *   the noncharacters U+FFFE and U+FFFF. The error message names the
 *   defect, shows the offending bytes as 0xNN, and gives the line and the
 *   column, both counted from 1: LF, CR and CRLF each end a line, and the
 *   column counts characters. The parser never repairs the input and never
 *   substitutes a character. An escape in a double-quoted scalar is the one
 *   way to put such a character into a value: "\x01", "\x7f", "\x80" and
 *   "\uFFFE" are c-printable text. The null character is the exception,
 *   as the note on null bytes above states. An escape that names no
 *   character (a lone UTF-16 surrogate in a \u escape, or a \U value that
 *   is a surrogate or above U+10FFFF) is a parse error; two \u escapes that
 *   form a surrogate pair name one character, as in JSON.
 * - A tag on a dictionary key. The dictionary keys of this DOM are plain C
 *   strings, not nodes (see cyaml_dictionary_get()). The tag types the key
 *   exactly as it types a value ("!!str 010" and "!!str 10" are two keys,
 *   "!!int abc" is a parse error), and the dictionary stores the canonical
 *   text of the typed key. There is no place to store the tag itself.
 * - The original %TAG shorthand spelling of a custom tag. A round trip
 *   through a parse and then a serialize does not keep that spelling.
 *   cyaml_serialize() and cyaml_serialize_flow() always write a custom tag
 *   in its fully resolved, verbatim (!<...>) form. The text of the tag does
 *   survive the round trip in full. Three bytes cannot appear literally
 *   inside !<...>: '>', a line break, and the '%' that starts an escape.
 *   The serializer writes each of them as a percent escape. A second parse
 *   of the output document therefore gives the identical tag string.
 * - A string that is not well-formed UTF-8 in the tree. Every scalar
 *   value, dictionary key and tag in a tree is well-formed UTF-8.
 *   cyaml_create_string(), cyaml_set(), cyaml_dictionary_set() and
 *   cyaml_node_set_tag() refuse a caller string that is not, and change
 *   nothing. They accept every Unicode character, those that a document
 *   may hold only as an escape included: the serializer writes a string
 *   that holds one double-quoted with an escape, and a tag with a
 *   percent-escape, so the output always parses back to the same text.
 *
 * ### Tags
 *
 * cyaml_node_tag() reads the tag of a node. cyaml_node_set_tag() sets it.
 * The doc comment of each function gives the details. This header defines a
 * named constant for each of the seven core-schema tag URIs of YAML 1.2:
 * CYAML_TAG_NULL, _BOOL, _INT, _FLOAT, _STR, _SEQ and _MAP. Use them so
 * that you do not type the URI strings by hand.
 *
 * There are five scalar core-schema tags: !!null, !!bool, !!int, !!float
 * and !!str. One of them forces its type on a plain, single-quoted,
 * double-quoted, literal-block or folded-block scalar. The forced type
 * replaces the type that the text of the scalar resolves to implicitly.
 * The four quoted and block styles have no implicit typing of their own
 * (see "Implicit core-schema typing" above). For those four styles, the tag
 * forces a type onto text that always becomes a plain string without it.
 *
 * !!bool, !!int and !!float each check the text of their own scalar. On a
 * mismatch the parser reports a hard failure for the whole document. It
 * gives no partial result and no fallback result. For example, !!int on
 * text that is not a valid integer fails this way. !!null is the one
 * exception. A null value carries no further information, so there is no
 * canonical "wrong" spelling for it. This is why !!null forces CYAML_NULL
 * on any text at all. A custom tag on a scalar behaves the same way and
 * never checks what it is attached to (see below). Two more cases are
 * always a hard parse failure, whatever scalar tag is present. The first is
 * one of the five scalar core-schema tags on real block or flow collection
 * syntax. The second is !!seq or !!map on the wrong collection kind, or on
 * a scalar.
 *
 * !!bool accepts a wider vocabulary than implicit bool typing does, and it
 * ignores letter case. That vocabulary holds "true" and "false", "yes" and
 * "no", and "on" and "off". It does not hold the single letters "y" and "n".
 * !!int and !!float also strip trailing whitespace before they parse. A
 * chomped trailing newline of a literal or folded block scalar therefore
 * never causes a false mismatch.
 *
 * A custom tag is any tag other than the seven core-schema URIs, and
 * !!binary is one of them. A custom tag never forces the type of a scalar.
 * The parser attaches it to the type that the scalar resolves to without
 * it. A plain scalar resolves that type implicitly. The four quoted and
 * block styles always resolve to a string. On a collection, a custom tag is
 * only metadata and has no effect on the parse. A bare "!" is the
 * non-specific tag, and it is not a shorthand tag. It also forces no type,
 * and it behaves in the same way as no tag at all.
 *
 * A node never carries a core-schema tag that names another type.
 * cyaml_node_set_tag() refuses such a tag, and cyaml_set() drops a
 * core-schema tag that the new value no longer matches. A custom tag
 * decorates a node of any type and survives every cyaml_set().
 *
 * ### Merge keys
 *
 * A mapping entry can have the literal, unquoted, untagged plain scalar
 * "<<" as its key. The parser then expands the value of that entry into the
 * same mapping in place. That value is one mapping, or a sequence of
 * mappings. After the expansion the parser removes the "<<" key itself.
 * Both key forms start this expansion: the implicit form ("<<: ...") and
 * the explicit form ("? <<" and ": ..."). The parser treats the two forms
 * in the same way, because both spell an unquoted, untagged plain scalar
 * that reads exactly "<<". An explicit key that the mapping already holds
 * always wins over a merged key. For a sequence of sources
 * ("<<: [*a, *b]"), an earlier source wins over a later one on a conflict.
 * The merged members take the place of the "<<" entry in the member order
 * of the mapping: they follow the explicit members before it and precede
 * the explicit members after it, source by source, each source in its own
 * member order. An explicit key keeps its own place.
 * A merge source that is neither a mapping nor a sequence of mappings is a
 * hard parse failure. The parser expands the "<<:" entry of a merged
 * mapping before it merges that mapping. A merge is therefore transitive,
 * and it needs no special handling.
 *
 * A "<<" key whose tag is the core merge tag, tag:yaml.org,2002:merge, is a
 * merge key too, whether it is plain or quoted. That tag may be written
 * !!merge, through a %TAG handle, or in the verbatim form. A key spelled
 * "<<" that is quoted with no tag ("<<"), or that carries any other tag
 * (!!str <<), is an ordinary literal key. The parser never expands it. This
 * matches every other core-schema type, which the parser resolves
 * implicitly only for a plain, untagged scalar, and otherwise takes from an
 * explicit tag.
 *
 * ### Implementation-defined behavior
 *
 * - Duplicate mapping keys. When a mapping holds the same key more than
 *   once, the last value wins. The parser replaces each earlier value
 *   silently. Common YAML parsers behave the same way, but the YAML 1.2
 *   specification does not demand it. The application must make sure that
 *   its input holds no duplicate key that it did not intend.
 * - Dictionary key order. A CYAML_DICTIONARY keeps its members in
 *   insertion order: the order of the parsed text, or the order of the
 *   cyaml_dictionary_set() and cyaml_set() calls that added each key. A
 *   replacement of the value of a key keeps its place, so a key that a
 *   document repeats keeps the place of its first occurrence and the value
 *   of its last. A removed key leaves the order, and a key added again goes
 *   to the end. cyaml_serialize(), cyaml_serialize_flow(), cyaml_clone()
 *   and cyaml_dictionary_first() all follow this order, so a parse and then
 *   a serialize keeps the key order of the source document, the same in
 *   every run. A merge key places its members as the "Merge keys" section
 *   describes. The element order of a CYAML_LIST is always exactly the
 *   order in which the caller pushed the elements or the parser read them.
 * - Nesting limits. The parser rejects a document that nests mappings,
 *   sequences or explicit keys more than 500 levels deep, with a parse
 *   error. There is a second, separate limit. The parser also rejects a
 *   non-scalar dictionary key (a sequence or a mapping) whose canonical
 *   flow-YAML text is larger than 64 KiB. This second limit bounds a key
 *   that is itself a mapping which holds another such key many levels deep.
 *   Canonicalization re-quotes the already quoted text of each enclosing
 *   level, so that text grows fast with the nesting depth. The parser builds
 *   that text only up to the limit, so a refused key costs at most 64 KiB
 *   of text whatever its shape, however many aliases it expands. No
 *   realistic document that a person writes or a program generates reaches
 *   either limit. cyaml_serialize(), cyaml_serialize_flow() and cyaml_clone()
 * each hold the same 500-level nesting limit on their own. A tree that the
 *   caller gives to one of these three need not come from cyaml_parse() at
 *   all. The caller can build it directly with cyaml_create_list() and
 *   cyaml_dictionary_set(). A serialize or a clone of a tree that is nested
 *   deeper than this limit therefore returns NULL. None of the three has a
 *   separate error-string channel, so the caller cannot tell this NULL from
 *   an allocation failure. The parse, the serialize and the clone all walk
 *   the tree with an explicit worklist, not by recursion. The call stack
 *   that each one needs is a constant that does not grow with the nesting
 *   depth. Every one of these limits bounds the work of a single call, not
 *   the stack that the caller must give.
 * - Node count limit. One cyaml_parse() or cyaml_parse_n() call rejects a
 *   document that needs more DOM nodes than the larger of 4,000,000 and 4
 *   for each byte of input. Of those, the nodes that the parse makes by
 *   copying (see "Alias expansion limit" below) may number at most
 *   4,000,000, whatever the input length. The parse error names the limit
 *   explicitly. It
 *   is not an out-of-memory message, and the amount of free memory does not
 *   change it. This limit does not apply to a tree that the caller builds
 *   directly with cyaml_create_list() and cyaml_dictionary_set() outside a
 *   parse.
 * - Document memory limit. One cyaml_parse() or cyaml_parse_n() call also
 *   rejects a document whose DOM would use more than the larger of 256 MiB
 *   and 512 bytes for each byte of input. The count covers the node structs,
 *   their backing containers, the text of every scalar, every tag and every
 *   dictionary key. The parse error again names the limit explicitly
 *   instead of a report of out of memory. This limit bounds what the node
 *   count alone cannot. A node is not a fixed amount of memory, and an
 *   alias deep-copies everything that its anchor holds. A small document
 *   whose bytes sit in a few large anchored scalars can therefore expand
 *   without bound while its node count stays low. An ordinary document of
 *   any size stays well below both limits. Like the node limit, it applies
 *   only to a parse, never to a tree that the caller builds directly
 *   through the public API.
 * - Alias expansion limit. The nodes and bytes that a parse makes by
 *   copying what it already built count against a fixed limit of 4,000,000
 *   nodes and 256 MiB, which does not grow with the input. A copy is the
 *   anchored node that an anchor registers, the node that an alias
 *   resolves to, a member that a merge key copies into its mapping, and the
 *   canonical text of a key that is a sequence or a mapping. These are the
 *   only ways in which a document makes more DOM than its text describes,
 *   so a nested-alias document meets this limit at the same point whether
 *   or not a long comment pads it. The copies count against the two limits
 *   above as well. The parse error names this limit.
 * - Integer range. CYAML_INTEGER is a signed 64-bit long long. An implicit
 *   decimal, hex or octal integer literal can hold a value that does not
 *   fit. It then falls back like a decimal literal that is too wide. It
 *   becomes a CYAML_FLOAT when the text also parses as a float. If it does
 *   not, it becomes a plain CYAML_STRING. The parser never silently makes it
 *   a CYAML_INTEGER that wrapped around or that has the wrong sign.
 * - Long keys in the output. An implicit key ("key: value") holds at most
 *   1024 characters under YAML 1.2, and PyYAML and libyaml refuse a longer
 *   one. cyaml_serialize() writes a key whose rendered text is longer than
 *   1024 bytes in the explicit form ("? key", then ": value" on the next
 *   line), and cyaml_serialize_flow() writes it as an explicit "{? key:
 *   value}" entry. The parser reads both forms back as the same entry.
 * - Numbers and the locale. The parser and both serializers always use '.'
 *   as the decimal point of a float, whatever LC_NUMERIC the process or the
 *   calling thread uses. For the length of one cyaml_parse*(),
 *   cyaml_serialize*() call, the calling thread
 *   runs in the "C" locale through uselocale(), which changes no other
 *   thread. The call restores the locale of the thread before it returns. A
 *   custom allocator that the call invokes runs inside that window.
 *
 * ### Custom memory management
 *
 * Every factory function and the parser take a ccol_memmgmt_procs_t *mp
 * parameter. Pass NULL to use the default malloc, free, calloc and realloc.
 * Each node stores its own allocator. All the nodes of one tree must carry
 * the same allocator.
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
 * **Thread-local node pool:** Each thread keeps a free-list pool of up to
 * 512 nodes. This pool makes the default-allocator case faster. A node with
 * a custom allocator does not use the pool. The pool drains by itself when
 * the thread stops.
 *
 * ### Path syntax (cyaml_get / cyaml_set)
 *
 * The syntax is the same as in cjson. A path has components that a dot
 * separates, for example "server.hosts.#0.port". A component that starts
 * with '#' and then has digits indexes into a list. It does this when the
 * current node is a CYAML_LIST. In every other case the library uses the
 * whole component, with the '#', as a literal dictionary key.
 *
 * A path string has two escape sequences. They let you address a key that
 * holds a literal '.' or '\'. The sequence '\.' resolves to a literal '.'
 * in the key, and not to a path separator. The sequence '\\' resolves to a
 * literal '\'. A '\' before any other character passes through unchanged.
 * For example, cyaml_get(doc, "a\\.b") addresses a key with the literal
 * name "a.b". cyaml_get(doc, "a\\.b.c\\.d") addresses the key "c.d" one
 * level under the key "a.b".
 *
 * ### Ownership
 *
 * - cyaml_create_*(), cyaml_parse*() and cyaml_clone() give trees that the
 *   caller owns in full.
 * - cyaml_list_push() and cyaml_dictionary_set() transfer ownership of the
 *   child to the parent on success. Do not free the child after such a
 *   call. One return code carries one rule about the child.
 *   ccol_invalid_args always means that the function rejected the arguments
 *   and did not touch the child at all, so the caller still owns it. Every
 *   other failure takes ownership of the child and deep-frees it. The child
 *   must not already be attached to a list parent or a dictionary parent. A
 *   fresh node is acceptable, and so is a fresh cyaml_clone(). A borrowed
 *   reference from cyaml_get(), cyaml_list_get() or cyaml_dictionary_get()
 *   is not acceptable. A node that is already attached would get two owners
 *   if the function accepted it. Each owner frees the node when its own
 *   parent is destroyed. The doc comment of each function lists every
 *   rejection and the one exception that does nothing.
 * - cyaml_get() gives a NON-OWNING reference. That reference stays valid
 *   until the caller changes or destroys the tree.
 * - cyaml_destroy() frees the whole subtree and sets the handle to NULL. It
 *   tears the tree down with a loop, not by recursion. A tree of any depth
 *   therefore never grows the native call stack when the library frees it.
 * - Every tree that cyaml_parse*() builds is acyclic by construction. A
 *   tree that the caller builds or changes directly with cyaml_list_push()
 *   or cyaml_dictionary_set() must also stay acyclic. Neither function
 *   checks for a cycle, for example a node that goes into its own subtree.
 *   A cyclic tree that the caller gives to cyaml_destroy(), cyaml_clone()
 *   or cyaml_serialize*() is undefined behavior.
 *
 * ### Serialization
 *
 * cyaml_serialize() gives block-style YAML.
 * cyaml_serialize_flow() gives compact flow-style YAML on one line.
 * The library allocates the buffer that it gives with the allocator of the
 * root node. Free that buffer with cyaml_serialize_free(). Give the same mp
 * that you used at parse time or at create time, or NULL for the default
 * allocator.
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

/** @brief Opaque DOM node type.  The full definition is in cyaml.c. */
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
 * YAML supports .inf and .nan explicitly, and cjson does not. The library
 * stores a value that is not finite, and it serializes that value as .inf,
 * -.inf or .nan.
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
 * @param val  Null-terminated, well-formed UTF-8 string. The node makes its
 *             own copy of it. NULL gives a CYAML_NULL node. Every Unicode
 *             character is accepted; a string that is not well-formed UTF-8
 *             is refused.
 * @param mp   Custom allocator, or NULL for default.
 * @return The new node, or NULL when val is not well-formed UTF-8 or an
 *         allocation fails.
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
 * The input must be UTF-8 text made only of the printable characters of
 * YAML 1.2 (see "Unsupported / explicitly out-of-scope" at the top of this
 * header). The function refuses any other input as a parse error whose
 * message gives the line and the column of the first offending character.
 *
 * @param yaml_str  Input text (must be null-terminated).
 * @param err_str   Out-parameter for the error message on a parse failure.
 *                  On success this function sets *err_str to NULL. On a
 *                  failure it points *err_str at a message that the LIBRARY
 *                  owns. Never free that string, and never free it with
 *                  cyaml_serialize_free_mp(): it is not an allocation. This
 *                  is the same rule that the err out-parameter of every
 *                  other module in this library follows. The text stays
 *                  valid until the next FAILING parse on the same thread,
 *                  which is the lifetime that strerror(3) and dlerror(3)
 *                  give. The storage is per-thread, so two threads that
 *                  parse at the same time never overwrite one another's
 *                  message. Copy the text if you need it past that point.
 *                  The return value always shows success or failure, and
 *                  *err_str never does. Pass NULL to ignore the error
 *                  details. The message is printable ASCII whatever the
 *                  input holds: where it quotes text of the input (a
 *                  scalar, a tag, an anchor or alias name, a character),
 *                  it writes a printable ASCII byte as it is and any other
 *                  byte as <0xNN>, and it cuts a long quoted text short at
 *                  a whole byte with "...". A character that the input
 *                  may not hold at all is named as 0xNN.
 * @param mp        Custom allocator for all the nodes of the tree that this
 *                  function builds, or NULL for the default allocator.
 * @return Root cyaml node on success, NULL on a parse failure.
 */
cyaml cyaml_parse_mp(const char *yaml_str, char **err_str,
                     ccol_memmgmt_procs_t *mp);

/** @brief Parse a null-terminated YAML string (default allocator). */
static inline cyaml cyaml_parse(const char *yaml_str, char **err_str) {
  return cyaml_parse_mp(yaml_str, err_str, NULL);
}

/**
 * @brief Parse a bounded YAML buffer (it need not be null-terminated).
 *
 * The function reads exactly @p len bytes, and the character rules of
 * cyaml_parse_mp() apply to every one of them: a null byte, any other
 * refused character, or a UTF-8 sequence that @p len cuts short is a parse
 * error. A byte past @p len is never read.
 *
 * @param yaml_str  Input buffer.
 * @param len       Number of bytes to parse.
 * @param err_str   Out-parameter for the error message. It behaves in the
 *                  same way as in cyaml_parse_mp, and it has the same
 *                  ownership rule: the library owns the string and the
 *                  caller never frees it. The message is printable ASCII
 *                  whatever the len bytes hold, as cyaml_parse_mp
 *                  describes. Pass NULL to ignore it.
 * @param mp        Custom allocator, or NULL for default.
 * @return Root cyaml node, or NULL on a failure.
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
 * This function writes a scalar in the plain form that reads best, when
 * that form is safe. If it is not safe, the function writes the scalar
 * double-quoted. The plain form is used only when YAML 1.1 readers such as
 * PyYAML and libyaml read it as the same string too, so a string that holds
 * a tab, a '?', or one of the YAML 1.1 line breaks NEL (U+0085), LS
 * (U+2028) and PS (U+2029), and the string "=", go out double-quoted, with
 * those line breaks written as their \N, \L and \P escapes. A list uses
 * the "- item" block syntax. A dictionary uses
 * the "key: value" block syntax, with the members in insertion order (see
 * "Implementation-defined behavior" > "Dictionary key order" above). A
 * float always carries a '.' in its mantissa ("1.0", "1.0e+20"), so that a
 * YAML 1.1 reader also reads it as a float, and its digits are the
 * shortest of 15, 16 or 17 significant digits that read back as the same
 * double.
 *
 * The library allocates the buffer that this function gives with the
 * allocator of the root node. Free that buffer with cyaml_serialize_free().
 * You can also use cyaml_serialize_free_mp() with the same mp that created
 * the tree.
 *
 * This function walks the tree with an explicit worklist, not by recursion.
 * The call stack that it needs is a constant that does not grow with the
 * nesting depth.
 *
 * @param node  Root of the tree or subtree to serialize. A NULL node
 *              serializes in the same way as a CYAML_NULL node ("~\n").
 *              This is a successful, non-NULL return, not a signal of
 *              failure.
 * @return A null-terminated YAML string on the heap. The function returns
 *         NULL when it runs out of memory, and also when node is nested
 *         more than 500 levels deep. See "Implementation-defined behavior"
 *         > "Nesting limits" above. The caller cannot tell the two causes
 *         apart, because this function has no separate error-string
 *         channel.
 */
char *cyaml_serialize(cyaml node);

/**
 * @brief Serialize a DOM tree to compact flow-style YAML.
 *
 * The output looks like JSON. A list becomes [a, b, c]. A dictionary
 * becomes {key: value}. This form is useful for a compact text on one line.
 * The members of a dictionary and the spelling of a float follow the same
 * rules as in cyaml_serialize() above. This function also walks
 * the tree with an explicit worklist, not by recursion, for the same
 * reason.
 *
 * @param node  Root of the tree or subtree to serialize. A NULL node
 *              serializes in the same way as a CYAML_NULL node ("~"). This
 *              is a successful, non-NULL return, not a signal of failure.
 * @return A null-terminated string on the heap. The function returns NULL
 *         when it runs out of memory, and also when node is nested more
 *         than 500 levels deep. See the same note on cyaml_serialize()
 *         above.
 */
char *cyaml_serialize_flow(cyaml node);

/**
 * @brief Serialize a list of documents as a multi-document YAML stream.
 *
 * This is the counterpart of the parse of a stream. cyaml_parse() gives a
 * CYAML_LIST with one element for each document of a stream that holds more
 * than one document, and this function writes such a list back as a stream:
 * each element becomes its own document, which starts with a "---" line and
 * holds the block-style YAML that cyaml_serialize() writes for that element.
 * A parse of the result of a list of two or more documents therefore gives
 * back an equal list (a parse of a one-document stream gives its root), and
 * a stream of Kubernetes manifests, for example, can be parsed, edited and
 * written back for a tool that reads it one document at a time.
 *
 * - An empty list gives an empty stream, the empty string "". A parse of an
 *   empty string gives a CYAML_NULL, not an empty list, because a stream
 *   with no documents and a document that is empty read the same.
 * - A node that is not a list, a NULL node included, is written as a stream
 *   of one document, so that the root of a one-document parse writes back
 *   as that same document. A list that is the root of a single document
 *   cannot be told apart from a stream of documents, in the parse as here;
 *   write such a document with cyaml_serialize().
 * - A tag of the list itself is not written, because a stream has no node
 *   that could carry it. A list that a parse of a stream gives never
 *   carries one.
 * - Each document stands alone. The output holds no directive and no
 *   anchor: a tag goes out in the same form as in cyaml_serialize(), which
 *   needs no %TAG directive, and an alias of the parsed input is already an
 *   independent copy in the tree.
 *
 * The library allocates the buffer with the allocator of @p node. Free it
 * with cyaml_serialize_free() or cyaml_serialize_free_mp().
 *
 * @param node  The list whose elements are the documents, or a single
 *              document root.
 * @return A null-terminated YAML string on the heap. The function returns
 *         NULL when it runs out of memory, and also when a document is
 *         nested more than 500 levels deep, as cyaml_serialize() does.
 */
char *cyaml_serialize_stream(cyaml node);

/**
 * @brief Free a string returned by cyaml_serialize(),
 *        cyaml_serialize_flow() or cyaml_serialize_stream().
 *
 * @param s   String to free. It can be NULL.
 * @param mp  The same allocator that was active when the library created
 *            the tree. Pass NULL when the tree used the default allocator.
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
 * @param node  It can be NULL, and the function then returns CYAML_NULL.
 */
cyaml_node_type_t cyaml_type(cyaml node);

/** @brief Return a string literal that names the type of @p node. */
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
 * Compare one of them against the value that cyaml_node_tag() returns, or
 * give one of them to cyaml_node_set_tag(). You then do not type the URI
 * strings by hand.
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
 * A node that the parser reads from a document with an explicit tag carries
 * the fully resolved tag string here. Such a tag is "!!str", a custom
 * "!foo", a shorthand that %TAG resolves, or a verbatim tag. A node with no
 * tag returns NULL. This includes a node whose type comes only from
 * implicit core-schema resolution. The tag is one of the seven CYAML_TAG_*
 * constants above, or a custom tag string. Which one it is depends on what
 * the source document, or cyaml_node_set_tag(), gave.
 *
 * A dictionary key can never carry a tag. The keys of this DOM are plain
 * strings, not nodes (see the doc comment of cyaml_dictionary_get()). A tag
 * on a key in the document types that key, exactly as it types a value, and
 * the dictionary stores the canonical text of the typed key. There is no
 * place to store the tag itself.
 *
 * @param node  It can be NULL, and the function then returns NULL.
 */
const char *cyaml_node_tag(cyaml node);

/**
 * @brief Attach or replace a node's tag, or clear it.
 *
 * This function sets metadata only. It never changes the type or the value
 * that the node already has. A core-schema tag (one of the seven
 * CYAML_TAG_* constants) names exactly one node type: CYAML_TAG_NULL names
 * CYAML_NULL, CYAML_TAG_BOOL names CYAML_BOOL, CYAML_TAG_INT names
 * CYAML_INTEGER, CYAML_TAG_FLOAT names CYAML_FLOAT, CYAML_TAG_STR names
 * CYAML_STRING, CYAML_TAG_SEQ names CYAML_LIST and CYAML_TAG_MAP names
 * CYAML_DICTIONARY. The function refuses a core-schema tag on a node of any
 * other type with ccol_invalid_args, because cyaml_parse() either rejects
 * that pairing or reads it back as a different type. A caller that wants a
 * string node must construct one with cyaml_create_string() and then tag
 * it. A custom tag is valid on a node of any type.
 *
 * A custom tag that you set here survives a later cyaml_set() call on the
 * same node. A core-schema tag survives such a call only while it still
 * names the type of the node. cyaml_set() drops a core-schema tag that the
 * new value no longer matches. For example, a node parsed from
 * "port: !!int 8080" loses its tag when cyaml_set() writes the string
 * "auto" to it, and keeps it when cyaml_set() writes the integer 9090.
 *
 * The empty string is not a valid tag, and this function rejects it with
 * ccol_invalid_args. The serializer writes a custom tag in its verbatim
 * "!<...>" form, and cyaml_parse() does not accept "!<>". An empty tag
 * would therefore give a document that this library cannot parse again.
 * Pass NULL to clear a tag. A tag must be well-formed UTF-8, and the
 * function refuses one that is not with ccol_invalid_args. It accepts every
 * other string. This includes a string that holds a character which cannot
 * appear literally inside "!<...>". The "Tags" section at the top of this
 * header describes how such a character survives a round trip.
 *
 * @param node  Target node of any type. For NULL the function reports
 *              ccol_invalid_args and does nothing else.
 * @param tag   New tag string, which the node copies. It must not be the
 *              empty string. NULL clears the tag that the node has.
 * @return ccol_success. The function returns ccol_invalid_args when node is
 *         NULL, when tag is the empty string, when tag is not well-formed
 *         UTF-8, and when tag is a core-schema tag that does not name the
 *         type of node. It returns
 *         ccol_not_enough_memory on an allocation failure. On every failure
 *         it leaves the tag that the node already has untouched.
 */
ccol_retval_t cyaml_node_set_tag(cyaml node, const char *tag);

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/**
 * @brief Return the boolean value.
 * It calls ccol_fatal_err() when the type of the node is not CYAML_BOOL.
 */
bool cyaml_bool_val(cyaml node);

/**
 * @brief Return the integer value.
 * It calls ccol_fatal_err() when the type of the node is not CYAML_INTEGER.
 */
long long cyaml_int_val(cyaml node);

/**
 * @brief Return the floating-point value.
 * It calls ccol_fatal_err() when the type of the node is not CYAML_FLOAT.
 */
double cyaml_double_val(cyaml node);

/**
 * @brief Return the string value.  The node owns it, so do not free it.
 * It calls ccol_fatal_err() when the type of the node is not CYAML_STRING.
 */
const char *cyaml_str_val(cyaml node);

/**
 * @brief Return the element count of a list node.
 * It calls ccol_fatal_err() when the type of the node is not CYAML_LIST.
 */
size_t cyaml_list_len(cyaml node);

/**
 * @brief Return the key count of a dictionary node.
 * It calls ccol_fatal_err() when the type of the node is not
 * CYAML_DICTIONARY.
 */
size_t cyaml_dictionary_size(cyaml node);

/* ========================================================================== */
/*                          LIST / DICTIONARY MANIPULATION                    */
/* ========================================================================== */

/**
 * @brief Append a child node to a list.
 *
 * Ownership of @p child transfers to @p seq. Do not free @p child after
 * this call.
 *
 * @p child must not already be attached to a list parent or a dictionary
 * parent. It must also not be @p seq itself. This function rejects each of
 * these two cases with ccol_invalid_args. It leaves @p child completely
 * untouched, and whatever @p child is already attached to still owns it. A
 * node that the function accepted in such a case would have two owners.
 * Each owner frees the node when its own parent is destroyed. A node that
 * you just created is always acceptable, and so is a fresh cyaml_clone(). A
 * borrowed reference from cyaml_get(), cyaml_list_get() or
 * cyaml_dictionary_get() is never acceptable, because each of those names a
 * node that already has a parent.
 *
 * A NULL @p child, a NULL @p seq and an @p seq that is not a CYAML_LIST
 * are rejected in the same way: ccol_invalid_args, with @p child untouched
 * and still owned by the caller.
 *
 * On every other failure, ownership still transfers. These failures are out
 * of memory and ccol_container_full. The function then deep-frees @p child
 * before it returns. ccol_invalid_args therefore always means that the
 * caller still owns @p child, and every other failure code means that it
 * does not.
 *
 * @p child must also not already contain @p seq, directly or through
 * another node. The function does not check this. A cycle that you make
 * this way turns a later cyaml_destroy(), cyaml_clone() or
 * cyaml_serialize*() on the tree into undefined behavior. The attachment
 * rule above excludes this case for every node that has a parent. What
 * stays unchecked is an ancestor with no parent, which is the root of the
 * tree that @p seq belongs to.
 *
 * @param seq    Target list node (it must be CYAML_LIST).
 * @param child  Child to append.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_container_full. ccol_container_full means that the list
 *         holds the largest element count that it can represent. A real
 *         program does not reach that count.
 */
ccol_retval_t cyaml_list_push(cyaml seq, cyaml child);

/**
 * @brief Access an element of a list by index (non-owning).
 * @return The child node. It returns NULL when index is out of bounds, and
 *         also when the type does not match.
 */
cyaml cyaml_list_get(cyaml seq, size_t index);

/**
 * @brief Set (insert or replace) a key in a dictionary.
 *
 * Ownership of @p child transfers to @p map. Do not free @p child after
 * this call.
 *
 * Every ccol_invalid_args leaves @p child untouched and still owned by the
 * caller. That covers a NULL @p child, a NULL @p map, a NULL @p key, a
 * @p key that is not well-formed UTF-8, an @p map that is not a
 * CYAML_DICTIONARY, and the two attachment rejections below. On every other
 * failure, which is out of memory or ccol_container_full, ownership still
 * transfers, and the function deep-frees @p child before it returns.
 *
 * When the key is already present, the function deep-frees the previous
 * child and then stores the new child in the same slot. Nothing between the
 * two steps can fail, so the slot never names freed memory once the call
 * returns, and the two children are never in memory at the same time. The
 * key keeps its place in the member order of the dictionary. A new key goes
 * after every member that the dictionary already holds. See
 * cyaml_dictionary_iter.
 *
 * @p child must not already be attached to a list parent or a dictionary
 * parent. It must also not be @p map itself. The function rejects each of
 * these two cases with ccol_invalid_args and leaves @p child completely
 * untouched. The doc comment of cyaml_list_push() gives the reason. There
 * is one exception: you can set @p key to the value that it already holds.
 * The function accepts the borrowed reference that this same slot owns. It
 * then does nothing and returns ccol_success.
 *
 * @p child must also not already contain @p map, directly or through
 * another node. The function does not check this. A cycle that you make
 * this way turns a later cyaml_destroy(), cyaml_clone() or
 * cyaml_serialize*() on the tree into undefined behavior.
 *
 * @param map    Target dictionary node (it must be CYAML_DICTIONARY).
 * @param key    Null-terminated, well-formed UTF-8 key string. The
 *               dictionary stores its own copy of it.
 * @param child  Value node.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_container_full. ccol_container_full means that the
 *         dictionary holds the largest element count that it can represent.
 *         A real program does not reach that count.
 */
ccol_retval_t cyaml_dictionary_set(cyaml map, const char *key, cyaml child);

/**
 * @brief Look up a key in a dictionary (non-owning).
 * @return The child node. It returns NULL when the key is absent, and also
 *         when the type does not match.
 */
cyaml cyaml_dictionary_get(cyaml map, const char *key);

/**
 * @brief Remove and deep-free the element at position index from a list.
 *
 * The function shifts every element after index left by one position. It
 * frees the removed subtree recursively.
 *
 * @param seq    Target list node (it must be CYAML_LIST).
 * @param index  Zero-based index of the element to remove.
 * @return ccol_success on success.
 *         ccol_invalid_args when seq is NULL, when seq is not a list, or
 *         when index is out of bounds.
 */
ccol_retval_t cyaml_list_remove(cyaml seq, size_t index);

/**
 * @brief Remove and deep-free the entry with the given key from a dictionary.
 *
 * The function frees the removed subtree recursively.
 *
 * @param map  Target dictionary node (it must be CYAML_DICTIONARY).
 * @param key  Null-terminated key string.
 * @return ccol_success on success.
 *         ccol_invalid_args when map is NULL or is not a dictionary.
 *         ccol_key_not_found when the key does not exist.
 */
ccol_retval_t cyaml_dictionary_remove(cyaml map, const char *key);

/* ========================================================================== */
/*                         DICTIONARY ITERATION                               */
/* ========================================================================== */

/**
 * @brief A cursor over the members of a dictionary, in insertion order.
 *
 * The caller owns the struct, usually on its own stack. The iteration
 * allocates nothing and cannot fail. Fill it with cyaml_dictionary_first()
 * and step it with cyaml_dictionary_next(). Read only @c key and @c value.
 * The fields whose names begin with an underscore are private.
 *
 * The order is the order in which each key first entered the dictionary: the
 * order of the members in the parsed text, or the order of the
 * cyaml_dictionary_set() and cyaml_set() calls that added them. A replacement
 * of the value of a key keeps its place, so a key that a document repeats keeps
 * the place of its first occurrence and the value of its last. A removed key
 * leaves the order. The members that a merge key ("<<") brings in take the
 * place of the "<<" entry, source by source in the order that the entry lists
 * them, each source in its own member order; a key that the mapping already
 * holds keeps its own place and value. The serializers and cyaml_clone() use
 * the same order.
 *
 * The cursor holds the member that the next step reads, which is the
 * successor of the member that the last successful call gave. It stays
 * valid across every change to the dictionary that leaves that successor
 * in place:
 *
 *   - a replacement of the value of any member, the current one included,
 *     through cyaml_dictionary_set() or cyaml_set(). The member keeps its
 *     place, so the iteration goes on in the same order;
 *   - the removal of the current member, or of any member other than the
 *     successor;
 *   - an insert of a new key. The new member goes after every other
 *     member. Whether this iteration still reaches it is unspecified.
 *
 * A removal of the successor, through cyaml_dictionary_remove(),
 * cyaml_delete() or any other call, invalidates the cursor, and so does a
 * destroy of the dictionary. Start again with cyaml_dictionary_first()
 * after such a change.
 *
 * The @c key and @c value fields describe the current member as it was
 * when the cursor stepped onto it. A replacement of the value of the
 * current member through cyaml_dictionary_set() destroys the old value, so
 * @c value then names freed memory; read the new value with
 * cyaml_dictionary_get() and @c key. cyaml_set() updates the existing
 * node in place, so @c value stays the same node.
 */
typedef struct cyaml_dictionary_iter {
  /** The key of the current member. It stays valid until that member is
   *  removed or the dictionary is destroyed. NULL when the iteration is
   *  over. */
  const char *key;
  /** The value of the current member. It is a borrowed reference, with the
   *  rules of cyaml_dictionary_get(). NULL when the iteration is over. */
  cyaml value;
  /** Private: the member that the next step reads. */
  const void *_cyaml_next;
} cyaml_dictionary_iter;

/**
 * @brief Start an iteration over a dictionary, and step onto its first
 *        member.
 *
 * @param dict  The dictionary node.
 * @param it    The cursor to fill. It must not be NULL.
 * @return true when @p it now stands on the first member. false when the
 *         dictionary is empty, when @p dict is NULL, or when @p dict is not
 *         a CYAML_DICTIONARY. The function then sets @c key and @c value to
 *         NULL. false also when @p it is NULL.
 *
 * Example:
 * @code
 * cyaml_dictionary_iter it;
 * for (bool ok = cyaml_dictionary_first(map, &it); ok;
 *      ok = cyaml_dictionary_next(&it)) {
 *   printf("%s: %s\n", it.key, cyaml_type_str(it.value));
 * }
 * @endcode
 */
bool cyaml_dictionary_first(cyaml dict, cyaml_dictionary_iter *it);

/**
 * @brief Step a dictionary cursor onto the next member.
 *
 * @param it  A cursor that cyaml_dictionary_first() filled.
 * @return true when @p it now stands on the next member. false when the
 *         previous call gave the last member, or when an earlier call
 *         already returned false. The function then sets @c key and
 *         @c value to NULL. false also when @p it is NULL.
 */
bool cyaml_dictionary_next(cyaml_dictionary_iter *it);

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/**
 * @brief Return a fully independent deep copy of a DOM subtree.
 *
 * The clone uses the same allocator as the source tree. It also carries over
 * the tag of each node unchanged, when the node has one (see cyaml_node_tag()),
 * and every dictionary of the clone keeps the member order of its source (see
 * cyaml_dictionary_iter). This function walks the source with an explicit
 * worklist, not by recursion. The call stack that it needs is a constant that
 * does not grow with the nesting depth.
 *
 * @param node  Root of the tree or subtree to clone. A NULL node gives
 *              NULL. cyaml_serialize() and cyaml_serialize_flow() differ
 *              here, because they treat a NULL node as a successful
 *              CYAML_NULL value. Here there is no tree to allocate a copy
 *              of.
 * @return The new root node, which the caller owns. The function returns
 *         NULL when node is itself NULL. It also returns NULL when it runs
 *         out of memory, and when node is nested more than 500 levels
 *         deep. See "Implementation-defined behavior" > "Nesting limits"
 *         above. The caller cannot tell these last two causes apart,
 *         because this function has no separate error-string channel.
 */
cyaml cyaml_clone(cyaml node);

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized cyaml variable.
 *
 * This macro exists so that the naming convention matches the other
 * container modules.
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
 * @brief Declare a cyaml variable with an automatic destroy at scope exit.
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

/** @brief Recursively free a DOM tree (internal).  Prefer the macro. */
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
 * You can call this macro on a variable that holds NULL, and it then does
 * nothing. @p node must be an assignable lvalue. You cannot give a plain
 * NULL literal directly, because this macro sets @p node to NULL after the
 * free. Use ___cyaml_destroy() directly when you do not have an lvalue.
 *
 * @note The macro evaluates node exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define cyaml_destroy(node)      \
  _ccol_cyaml_destroy_impl(node, \
                           _ccol_uniq(__ccol_cyaml_destroy_slot, __COUNTER__))

/* Internal. The body of cyaml_destroy. slot is a name from _ccol_uniq(), so
 * the macro nests inside the argument of another destroy macro and stays
 * -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_cyaml_destroy_impl(node, slot) \
  do {                                       \
    __typeof__(node) *slot = &(node);        \
    if (*slot) {                             \
      __cyaml_destroy(*slot);                \
      *slot = NULL;                          \
    }                                        \
  } while (0)

/* ========================================================================== */
/*                         PATH NAVIGATION; BACK-END                       */
/* ========================================================================== */

/**
 * @brief Navigate to the node addressed by a dot-separated path (back-end).
 *
 * Prefer the cyaml_get() macro.
 *
 * A NULL path or an empty path returns root itself. A path with no
 * component addresses the root. This matches cyaml_set() and
 * cyaml_delete(), which read a path that is only a leaf, for example "key",
 * as a child of root. Such a path needs no leading part at all.
 *
 * @return The target node. It returns NULL when root is NULL. It also
 *         returns NULL when a path component is missing or has the wrong
 *         type. It returns NULL as well when the path holds an empty
 *         component. An empty component is nothing between two dots, a
 *         leading dot, or a trailing dot.
 */
cyaml _cyaml_get(cyaml root, const char *path);

/**
 * @brief Remove and deep-free the node addressed by a path (back-end).
 *
 * Prefer the cyaml_delete() macro.
 *
 * This function navigates to the parent of the node that the path
 * addresses. It then calls cyaml_dictionary_remove() or
 * cyaml_list_remove(), whichever fits the parent. The path uses the same
 * dot-separated syntax as _cyaml_get and _cyaml_set_typed, with the same
 * escape sequences.
 *
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL or empty path, a wrong parent type,
 *         a bad "#N" index syntax, or an empty path component. An empty
 *         component is nothing between two dots, a leading dot, or a
 *         trailing dot. Any path component can cause this, not only the
 *         leaf.
 *         ccol_key_not_found when a path component with correct syntax is
 *         absent. Such a component is a dictionary key or a "#N" index. Any
 *         position can cause this. A list index with correct syntax that is
 *         out of range causes it too.
 *         ccol_not_enough_memory on an allocation failure.
 */
ccol_retval_t _cyaml_delete(cyaml root, const char *path);

/**
 * @brief Write a typed scalar value to the leaf addressed by a path (back-end).
 *
 * Prefer the cyaml_set() macro.
 *
 * For a dictionary leaf, this function creates the key when it is absent.
 * The parent dictionary must already exist. For a list leaf ("#N"), the
 * index must already be in range. A list has no way to grow by itself to
 * fit an index that the caller chooses. In both cases the function replaces
 * the value at the leaf and deep-frees the old value, whole subtrees
 * included.
 *
 * @param root             Root of the DOM tree.
 * @param path             Dot-separated path string.
 * @param type             YAML type of the new value.
 * @param raw              Pointer to the raw C value (passed as void *). It
 *                         may be NULL only for CYAML_NULL; the function
 *                         rejects a NULL raw for every other type with
 *                         ccol_invalid_args.
 * @param raw_size         sizeof() of the original C expression. It must
 *                         match the object that @p raw names. For
 *                         CYAML_INTEGER it is 1, 2, 4 or 8. For CYAML_FLOAT
 *                         it is sizeof(float) or sizeof(double). For
 *                         CYAML_BOOL it is sizeof(bool). For
 *                         CYAML_STRING it is sizeof(const char *), where
 *                         @p raw names a pointer to the string and not the
 *                         string itself. The function rejects every other
 *                         size with ccol_invalid_args. It does so before it
 *                         reads the value, so it leaves the target leaf
 *                         untouched.
 * @param is_signed        Whether the integer source type is signed.
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL or empty path, a wrong parent type,
 *         a bad "#N" index syntax, an empty path component, a CYAML_STRING
 *         value that is not well-formed UTF-8, or a dictionary leaf
 *         component that is not well-formed UTF-8. Each of these leaves the
 *         tree unchanged. An empty
 *         component is nothing between two dots, a leading dot, or a
 *         trailing dot. Any path component can cause this, not only the
 *         leaf.
 *         ccol_key_not_found when a list index has correct syntax but is
 *         out of range. That index can be at the leaf or above it. This
 *         code also comes back when a dictionary key above the leaf is
 *         absent, because the function needs that key to resolve the parent
 *         path. A dictionary key that is absent at the leaf itself is not
 *         an error, because the function creates it.
 *         ccol_not_enough_memory on an allocation failure.
 *         ccol_container_full when a new leaf dictionary key would take the
 *         parent past the largest element count that it can represent. A
 *         real program does not reach that count.
 */
ccol_retval_t _cyaml_set_typed(cyaml root, const char *path,
                               cyaml_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed);

/* ========================================================================== */
/*                         COMPILE-TIME TYPE HELPERS                         */
/* ========================================================================== */

/**
 * @brief Sentinel that _cyaml_type_of() gives for an unsupported C type.
 *
 * cyaml_set() documents the types that it accepts: bool, any integer type,
 * float, double, char *, const char *, and a bare NULL. _cyaml_type_of()
 * gives this sentinel for every other type. Examples are long double, a
 * struct, and any other type that the default association of _Generic below
 * catches.
 *
 * A bare `NULL` literal has the type void *, and it gets its own explicit
 * association below instead of this default. The C23 nullptr gets the same
 * result through _CCOL_NULLPTR_ASSOC. This is deliberate, because a
 * NULL is documented, intentional usage and not a mistake of the caller.
 * The doc comment of cyaml_set says so.
 *
 * This sentinel is deliberately NOT a real cyaml_node_type_t enumerator. No
 * node stores it, and cyaml_type() never returns it. It only passes through
 * the `type` argument of _cyaml_set_typed(). That function already has a
 * switch that checks the type, in node_reinit_scalar() in cyaml.c. The
 * switch rejects every value outside {CYAML_NULL, CYAML_BOOL,
 * CYAML_INTEGER, CYAML_FLOAT, CYAML_STRING} with ccol_invalid_args, and it
 * does so before it touches the target node. A value that differs from
 * every real enumerator is what makes that guard fire for an unsupported
 * type. A default of CYAML_NULL is already on the accept list, so it would
 * pass the guard silently.
 */
#define _CYAML_TYPE_UNSUPPORTED ((cyaml_node_type_t)0x7f)

/**
 * @brief Map the compile-time type of a C expression to cyaml_node_type_t.
 */
#if defined __clang__
#define _cyaml_type_of(val) \
  _cyaml_type_of_impl((val), _ccol_uniq(_cyt, __COUNTER__))
#define _cyaml_type_of_impl(val, _cyt_name)                                 \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    cyaml_node_type_t _cyt_name = _Generic((val),                           \
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
        _CCOL_NULLPTR_ASSOC(CYAML_NULL) void *: CYAML_NULL,                 \
        default: _CYAML_TYPE_UNSUPPORTED);                                  \
    _Pragma("GCC diagnostic pop");                                          \
    _cyt_name;                                                              \
  })
#else
#define _cyaml_type_of(val)                               \
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
      _CCOL_NULLPTR_ASSOC(CYAML_NULL) void *: CYAML_NULL, \
      default: _CYAML_TYPE_UNSUPPORTED)
#endif

/* Compile-time constant.  It is true when a plain 'char' is signed on this
 * platform. */
#if defined(__CHAR_UNSIGNED__)
#define _CYAML_CHAR_SIGNED false
#else
#define _CYAML_CHAR_SIGNED true
#endif

#if defined __clang__
#define _cyaml_is_signed(val) \
  _cyaml_is_signed_impl((val), _ccol_uniq(_cyis, __COUNTER__))
#define _cyaml_is_signed_impl(val, _cyis_name)                              \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool _cyis_name = _Generic((val),                                       \
        char: _CYAML_CHAR_SIGNED,                                           \
        signed char: true,                                                  \
        short: true,                                                        \
        int: true,                                                          \
        long: true,                                                         \
        long long: true,                                                    \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    _cyis_name;                                                             \
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
 * @param root  Root cyaml handle. The top node is a CYAML_DICTIONARY or a
 *              CYAML_LIST.
 * @param path  Dot-separated path. It is a string literal or a char *.
 * @return A non-owning cyaml handle, or NULL when the path does not exist.
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
 * The macro accepts these value types: bool, any integer type, float,
 * double, char *, const char *, a char array and a string literal. A NULL,
 * or the C23 nullptr, sets the leaf to CYAML_NULL. Before C23 the macro
 * `true` is the int 1, so cyaml_set(doc, k, true) stores the CYAML_INTEGER 1;
 * write (bool)true for a CYAML_BOOL. In C23 `true` has the type bool and
 * needs no cast. An integer becomes a CYAML_INTEGER, except
 * an unsigned value above LLONG_MAX, which no long long holds: it becomes a
 * CYAML_FLOAT with the nearest double, which is the node that
 * cyaml_parse() gives for its decimal literal.
 *
 * A string value, and a leaf component that addresses a dictionary key,
 * must be well-formed UTF-8; the macro returns ccol_invalid_args for one
 * that is not and changes nothing.
 *
 * For a dictionary leaf, the macro creates the key when it is absent. The
 * parent dictionary must already exist. For a list leaf ("#N"), the index
 * must already be in range. A list has no way to grow by itself to fit an
 * index that the caller chooses. The doc comment of _cyaml_set_typed()
 * gives the exact split between ccol_key_not_found and ccol_invalid_args
 * that this rule causes. When the leaf already exists, the macro always
 * changes its type. A custom tag on that leaf survives. A core-schema tag
 * survives only while it still names the type of the new value, and the
 * macro drops it otherwise (see cyaml_node_set_tag()).
 *
 * @param root  Root cyaml handle.
 * @param path  Dot-separated path string.
 * @param val   C value. _Generic detects its type at compile time. A
 *              string literal, a `char[N]` array and a pointer to char all
 *              give a CYAML_STRING, whatever their qualifiers. The macro
 *              copies val into a local whose type is the type of val after
 *              array-to-pointer conversion, with top-level qualifiers
 *              removed, so an array arrives as a pointer to its first
 *              character.
 * @return ccol_retval_t: ccol_success on success, error code otherwise.
 *
 * Example:
 * @code
 * cyaml_set(doc, "server.port", 8080);
 * cyaml_set(doc, "server.name", "prod");
 * cyaml_set(doc, "server.tls",  (bool)true);
 * @endcode
 */
#define cyaml_set(root, path, val) \
  _cyaml_set_impl((root), (path), (val), _ccol_uniq(_cyaml_sv, __COUNTER__))

/* The public macro above names the temporary of this body with _ccol_uniq(),
 * so cyaml_set() nests inside its own argument, and inside the argument of any
 * other public macro, under -Wshadow. */
#define _cyaml_set_impl(root, path, val, _cyaml_sv_name)              \
  ({                                                                  \
    __auto_type _cyaml_sv_name = (val);                               \
    _cyaml_set_typed((root), (path), _cyaml_type_of(_cyaml_sv_name),  \
                     (void *)&_cyaml_sv_name, sizeof(_cyaml_sv_name), \
                     _cyaml_is_signed(_cyaml_sv_name));               \
  })

/**
 * @brief Remove and deep-free the DOM node addressed by a dot-separated path.
 *
 * The macro navigates to the parent of the node that the path addresses. It
 * then removes the child and frees it recursively. For a dictionary parent,
 * the path addresses the leaf by key. For a list parent, the leaf must be a
 * '#N' component.
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

#pragma GCC visibility pop
