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

/* Everything this header declares from here to its end belongs to the public
 * Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of it. We build the library with -fvisibility=hidden,
 * so a function or object outside these blocks stays internal: its name isn't
 * in the dynamic symbol table, an application that links against the library
 * can't interpose it, and a symbol of the same name in that application can't
 * collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cyaml.h
 * @brief YAML 1.2 parser, serializer, and mutable in-memory DOM.
 *
 * The DOM is designed like the one of the cjson module: every node is an
 * opaque struct on the heap, a cvector holds the elements of a list, and a
 * chashmap holds the entries of a dictionary.
 *
 * ### Supported YAML features
 *
 * - Block dictionaries and lists, and flow dictionaries and lists.
 * - All scalar styles: plain, single-quoted, double-quoted, literal (|) and
 *   folded (>), with all chomping modes (clip, strip, keep). A plain,
 *   single-quoted or double-quoted scalar can span several physical lines,
 *   and the parser folds it with the same rules as a folded block scalar,
 *   so one line break folds to a space and a run of blank lines folds to
 *   that many newlines. An implicit mapping key must fit on one physical
 *   line, though; only an implicit *value* or an explicit key (? key) can
 *   span several lines.
 * - Anchors (&name) and aliases (*name).
 * - Implicit core-schema typing for null, bool, integer (decimal, unsigned
 *   0o octal or unsigned 0x hex) and float, where a float includes .inf and
 *   .nan. A decimal float too large for a double ("1e400", "-1.0e400") is an
 *   infinity, plain or with the !!float tag, and one too small becomes the
 *   nearest subnormal double or zero.
 * - Non-scalar dictionary keys. The dictionaries of this DOM always map
 *   char* -> cyaml, so the parser canonicalizes a sequence, mapping or flow
 *   collection used as a key straight into compact flow-style text. It
 *   writes the entries of a mapping in sorted order at every nesting level,
 *   lexicographic by key and independent of the insertion order, so two
 *   non-scalar keys with the same structure always canonicalize to the same
 *   text and collide correctly as one key, whatever order the caller built
 *   them in. The element order of a sequence always matters and the parser
 *   never reorders it.
 * - Scalar dictionary keys. The parser resolves the core-schema type of a
 *   plain key and stores its canonical text: a null key gives "null", a
 *   bool key "true" or "false", and an integer key its decimal text, so
 *   "0x10" gives "16". A float key gives the shortest decimal text that
 *   reads back as the same double, so "3.10" gives "3.1" and "1e300" gives
 *   "1e+300", and a subnormal double gets only as many digits as it needs,
 *   so "4.9406564584124654e-324" gives "5e-324". ".inf", "-.inf" and ".nan"
 *   keep those spellings, both zeros give "0", and an integral float has no
 *   fractional part, so the keys 1.0 and 1 are one key. "yes", "no", "on"
 *   and "off" are strings under YAML 1.2 and keep their own text. A quoted
 *   key with no tag keeps its text exactly, and a tag on a key types it
 *   first, the same way it types a value. The serializers write a key that
 *   is exactly the canonical text of an integer or a float without quotes,
 *   and quote every other key that a parse would read as another type or
 *   another text.
 * - Plain scalars and indicators. A plain scalar never starts with an
 *   indicator character, so outside a flow collection "a: ]", "]: x" and
 *   "- ,a" are parse errors, as YAML 1.2 section 7.3.3 requires. Inside a
 *   flow collection a plain scalar ends at every flow indicator, '[' and
 *   '{' included, so "[a{b]" is a parse error (a missing ','). Quote such
 *   text. A comment may end the line after an anchor or a tag inside a
 *   flow collection, and the node then follows on a later line.
 * - A flow collection spread over several lines. Every continuation line
 *   must be indented more than the block mapping key or the "- " that holds
 *   the collection. Indentation counts spaces only and tabs after those
 *   spaces are separation, so JSON indented with tabs parses at the top
 *   level. A line that starts with the closing ']' or '}' may sit exactly at
 *   that indentation, after spaces only, which closes the collection under
 *   the line that opened it.
 * - Multi-document streams delimited by "---". The first "---" is optional
 *   for an input with one document. With more than one document the parser
 *   gives a CYAML_LIST with one element per document root, and with one
 *   document it gives that document's root node directly. A later document
 *   can leave out its own "---" only when the document right before it
 *   ended with an explicit "...". A document that carries its own %YAML
 *   directive always needs an explicit "..." before it, even when it has
 *   its own "---".
 * - The %YAML directive. It is checked against the MAJOR.MINOR grammar, and
 *   a document can hold at most one. A major version other than 1
 *   ("%YAML 2.0") is a parse error, as YAML 1.2 section 6.8.1 requires. Any
 *   minor version is accepted and changes nothing, since every input is
 *   read as YAML 1.2 whatever version it declares. A directive needs a
 *   name, so a "%" with no name after it is a parse error; a directive with
 *   any other name is ignored.
 * - The tab character, as YAML 1.2 defines it. Indentation is spaces only,
 *   and a tab is separation whitespace wherever the grammar allows
 *   separation: after a "-", "?" or ":" indicator ("key:\tvalue"), after the
 *   spaces that indent a line, inside a flow collection, after "---" or
 *   "...", between the parts of a directive, and before a comment. A tab is
 *   refused where it would decide a block-structural column: in front of a
 *   block mapping key or a block sequence "-" on its line ("\ta: 1",
 *   "key:\t- a", "-\tb: c"), and as the indentation of a block scalar.
 *   Inside a scalar a tab is content. Since indentation counts spaces only,
 *   a line indented by tabs alone is indented by zero spaces.
 * - A UTF-8 byte order mark in the prefix of a document: at the start of
 *   the stream, at the start of a line before a later document (after a
 *   "..." or before a "---"), or at the end of the stream. A prefix may
 *   hold several marks with comment lines between them. The mark isn't
 *   content and a comment may follow it directly. YAML 1.2 section 5.2
 *   forbids a mark inside a document, so a mark at the start of a line
 *   anywhere else is a parse error; a file that ends without a "..."
 *   concatenated with a file that starts with a mark needs a "---" after
 *   the mark. A mark in the middle of a line is ordinary content.
 * - Tags (!!str, !!binary, a custom !foo, a shorthand that %TAG resolves,
 *   or a verbatim !<...> tag). The parser resolves them and
 *   cyaml_node_tag() reads them back; the "Tags" section below has the full
 *   rules.
 * - %TAG directive scoping for a shorthand prefix. The directives of each
 *   document are tracked separately, so a %TAG directive in one document
 *   never changes another one. A shorthand tag with an undefined handle is
 *   a parse error.
 * - Merge keys (<<:). A "<<:" entry of a mapping expands the mappings it
 *   names into that mapping in place, when its key reads exactly "<<" and is
 *   either a plain, untagged scalar or carries the core merge tag
 *   (!!merge). The "Merge keys" section below gives the rules.
 *
 * ### Unsupported / explicitly out-of-scope
 *
 * - Null bytes inside strings. The scalar value of a node is a plain
 *   null-terminated C string with no separate length, so it can never hold
 *   a null byte; don't put one into a string value. Every double-quoted
 *   escape that decodes to codepoint zero ("\0", "\x00", "\u0000",
 *   "\U00000000") is a parse error instead of a silent truncation of the
 *   value. Encode data that can contain a null byte as base64. For the same
 *   reason !!binary gets no special decoding: its base64 text is stored as
 *   an ordinary string with the tag "tag:yaml.org,2002:binary" and is never
 *   decoded into raw bytes.
 * - Any input that isn't UTF-8 text made only of the printable characters
 *   of YAML 1.2 section 5.1 (c-printable): TAB, LF, CR, 0x20 to 0x7E, NEL
 *   (U+0085), U+00A0 to U+D7FF, U+E000 to U+FFFD and U+10000 to U+10FFFF.
 *   Before parsing anything the parser checks every byte of the stream
 *   (scalars of every style, keys, comments, tags, anchors, directives and
 *   the space between tokens) and refuses the whole document at the first
 *   byte that breaks the rule. Such a byte belongs to a sequence that isn't
 *   well-formed UTF-8 (a truncated sequence, a continuation byte with no
 *   lead byte, an overlong form, an encoded surrogate, a code point above
 *   U+10FFFF, a byte from 0xF5 to 0xFF), or it is a C0 control other than
 *   TAB, LF and CR, DEL, a C1 control (U+0080 to U+009F) other than NEL, or
 *   one of the noncharacters U+FFFE and U+FFFF. The error message names the
 *   defect, shows the offending bytes as 0xNN, and gives the line and the
 *   column, both counted from 1: LF, CR and CRLF each end a line, and the
 *   column counts characters. The input is never repaired and no character
 *   is ever substituted. An escape in a double-quoted scalar is the one way
 *   to put such a character into a value, since "\x01", "\x7f", "\x80" and
 *   "\uFFFE" are c-printable text; the null character is the exception, as
 *   the note on null bytes above says. An escape that names no character (a
 *   lone UTF-16 surrogate in a \u escape, or a \U value that is a surrogate
 *   or above U+10FFFF) is a parse error, while two \u escapes that form a
 *   surrogate pair name one character, as in JSON.
 * - A tag on a dictionary key. The dictionary keys of this DOM are plain C
 *   strings and not nodes (see cyaml_dictionary_get()). The tag types the
 *   key exactly as it types a value ("!!str 010" and "!!str 10" are two
 *   keys, "!!int abc" is a parse error), and the dictionary stores the
 *   canonical text of the typed key, but there's nowhere to keep the tag
 *   itself.
 * - The original %TAG shorthand spelling of a custom tag. A parse followed
 *   by a serialize doesn't keep that spelling, because cyaml_serialize()
 *   and cyaml_serialize_flow() always write a custom tag in its fully
 *   resolved, verbatim (!<...>) form. The text of the tag survives the
 *   round trip in full: every byte that may not appear literally inside
 *   !<...> (any byte outside the ns-uri-char set of YAML 1.2, the '%' that
 *   starts an escape and a '#' included) is written as a percent escape, so
 *   parsing the output again gives the identical tag string.
 * - A string in the tree that isn't well-formed UTF-8. Every scalar value,
 *   dictionary key and tag in a tree is well-formed UTF-8, because
 *   cyaml_create_string(), cyaml_set(), cyaml_dictionary_set() and
 *   cyaml_node_set_tag() refuse a caller string that isn't, and change
 *   nothing. They accept every Unicode character, including those that a
 *   document may only hold as an escape: the serializer writes a string
 *   that holds one double-quoted with an escape, and a tag with a
 *   percent-escape, so the output always parses back to the same text.
 *
 * ### Tags
 *
 * cyaml_node_tag() reads the tag of a node and cyaml_node_set_tag() sets
 * it; the doc comment of each function has the details. This header defines
 * a named constant for each of the seven core-schema tag URIs of YAML 1.2
 * (CYAML_TAG_NULL, _BOOL, _INT, _FLOAT, _STR, _SEQ and _MAP), so you don't
 * have to type the URI strings by hand.
 *
 * There are five scalar core-schema tags: !!null, !!bool, !!int, !!float
 * and !!str. Each one forces its type on a plain, single-quoted,
 * double-quoted, literal-block or folded-block scalar, in place of the type
 * that the scalar's text resolves to implicitly. The four quoted and block
 * styles have no implicit typing of their own (see "Implicit core-schema
 * typing" above), so on those styles the tag forces a type onto text that
 * would otherwise always become a plain string.
 *
 * !!bool, !!int and !!float each check the text of their scalar, and a
 * mismatch is a hard failure for the whole document, with no partial or
 * fallback result; !!int on text that isn't a valid integer fails this way,
 * for example. !!null is the one exception: a null value carries no further
 * information, so there is no canonical "wrong" spelling for it, and !!null
 * forces CYAML_NULL on any text at all. A custom tag on a scalar behaves the
 * same way and never checks what it is attached to (see below). Two more
 * cases are always a hard parse failure whatever scalar tag is present: one
 * of the five scalar core-schema tags on real block or flow collection
 * syntax, and !!seq or !!map on the wrong collection kind or on a scalar.
 *
 * !!bool accepts a wider vocabulary than implicit bool typing and ignores
 * letter case: "true" and "false", "yes" and "no", and "on" and "off", but
 * not the single letters "y" and "n". !!int and !!float also strip trailing
 * whitespace before they parse, so the chomped trailing newline of a
 * literal or folded block scalar never causes a false mismatch. They never
 * strip anything in front of the number: the text must begin with a digit
 * or a sign (or, for !!float, a "."), so a space, tab, line break, vertical
 * tab or form feed in front of it, which a double-quoted scalar can carry
 * through an escape, is a mismatch.
 *
 * A custom tag is any tag other than the seven core-schema URIs, and
 * !!binary is one of them. A custom tag never forces the type of a scalar:
 * it is attached to whatever type the scalar resolves to without it, which
 * a plain scalar resolves implicitly and the four quoted and block styles
 * always resolve to a string. On a collection a custom tag is only metadata
 * and doesn't affect the parse. A bare "!" is the non-specific tag of YAML
 * 1.2 section 6.9.1 and not a shorthand tag: a plain scalar under it is a
 * string whatever its text, so "! 42" is the string "42" and "! 0x11: v"
 * has the key "0x11", while a collection keeps the kind that its syntax
 * gives. No node carries it, so cyaml_node_tag() gives NULL for such a
 * node. A written tag that resolves to "!" alone, such as the verbatim
 * "!<!>", is a parse error.
 *
 * The text of a written tag follows the character rules of YAML 1.2
 * sections 5.6 and 6.9.1. The content of a verbatim tag and the prefix of a
 * %TAG directive may only hold ns-uri-char: ASCII letters and digits, and
 * the punctuation -#;/?:@&=+$,_.!~*'()[]. The suffix of a shorthand tag may
 * hold the same set minus '!', ',', '[' and ']'. Any other byte (a space, a
 * quote, '<', '{', a byte of a non-ASCII character, and so on) is a parse
 * error that names it; write it as a percent escape such as "%7B". The
 * percent escapes of the verbatim content, of the shorthand suffix and of
 * the %TAG prefix are all decoded the same way, so "!e!a" under the prefix
 * "tag:x%21," and the verbatim "!<tag:x%21,a>" both give the tag
 * "tag:x!,a". An escape must decode to well-formed UTF-8 with no NUL in it.
 *
 * A node never carries a core-schema tag that names another type:
 * cyaml_node_set_tag() refuses such a tag, and cyaml_set() drops a
 * core-schema tag that doesn't match the new value. A custom tag can
 * decorate a node of any type and survives every cyaml_set().
 *
 * ### Merge keys
 *
 * When a mapping entry has the literal, unquoted, untagged plain scalar
 * "<<" as its key, the parser expands the value of that entry (one mapping,
 * or a sequence of mappings) into the same mapping in place, and the "<<"
 * key itself never becomes a member. Both key forms trigger the expansion,
 * the implicit one ("<<: ...") and the explicit one ("? <<" and ": ..."),
 * and they are handled the same way because both spell an unquoted,
 * untagged plain scalar that reads exactly "<<". A mapping can hold several
 * merge keys and every one of them expands, so two merge keys never replace
 * each other. Precedence follows PyYAML: an explicit key of the mapping
 * always wins over a merged key, of two merge keys the later one wins, and
 * for a sequence of sources ("<<: [*a, *b]") an earlier source wins over a
 * later one. A merged member takes the place of the first merge key that
 * brings its key, in the member order of the mapping: it comes after the
 * explicit members before that merge key and before the explicit members
 * after it, source by source, each source in its own member order. An
 * explicit key keeps its own place.
 * A merge source that is neither a mapping nor a sequence of mappings is a
 * hard parse failure. The "<<:" entry of a merged mapping is expanded
 * before that mapping is merged, so merges are transitive with no special
 * handling.
 *
 * A "<<" key whose tag is the core merge tag, tag:yaml.org,2002:merge, is a
 * merge key too, plain or quoted, and the tag may be written !!merge,
 * through a %TAG handle, or in verbatim form. A key spelled "<<" that is
 * quoted with no tag ("<<"), or that carries any other tag (!!str <<), is an
 * ordinary literal key, which is never expanded and stays a member next to
 * the members of every merge key. That's how every other core-schema type
 * works as well: it is resolved implicitly only for a plain, untagged
 * scalar and otherwise taken from an explicit tag.
 *
 * ### Implementation-defined behavior
 *
 * - Duplicate mapping keys. When a mapping holds the same key more than
 *   once the last value wins, and each earlier value is replaced silently.
 *   Common YAML parsers do the same, although the YAML 1.2 specification
 *   doesn't demand it. Since the replacement is silent, the application
 *   must make sure its input holds no duplicate key that it didn't intend.
 * - Dictionary key order. A CYAML_DICTIONARY keeps its members in insertion
 *   order: the order of the parsed text, or the order of the
 *   cyaml_dictionary_set() and cyaml_set() calls that added each key.
 *   Replacing the value of a key keeps its place, so a key that a document
 *   repeats keeps the place of its first occurrence and the value of its
 *   last. A removed key leaves the order, and a key added again goes to the
 *   end. cyaml_serialize(), cyaml_serialize_flow(), cyaml_clone() and
 *   cyaml_dictionary_first() all follow this order, so a parse and then a
 *   serialize keeps the key order of the source document, the same in every
 *   run. A merge key places its members as the "Merge keys" section
 *   describes. The element order of a CYAML_LIST is always exactly the order
 *   in which the caller pushed the elements or the parser read them.
 * - Nesting limits. A tree may nest at most 500 containers (sequences and
 *   mappings) on any path from its root, the root counted when it is a
 *   container. Scalars, tags and anchors take no level, so 500 nested
 *   containers are accepted with anything inside the innermost one, and a
 *   501st container is refused. The parser counts every container it
 *   builds, a "key: value" pair inside a flow sequence included since that
 *   is a one-entry mapping, and it counts the containers that an alias
 *   copies below the place where the alias stands; the value of a merge key
 *   counts there too. A collection used as a dictionary key counts while it
 *   is being read, below the mapping that holds the key. When a stream holds
 *   several documents the parse gives a list that holds them, so each
 *   document may then nest at most 499 containers. A document that goes past
 *   any of these is a parse error that names the limit.
 *   A second, separate limit rejects a non-scalar dictionary key (a
 *   sequence or a mapping) whose canonical flow-YAML text is larger than
 *   64 KiB. It bounds a key that is itself a mapping holding another such
 *   key many levels deep: canonicalization re-quotes the already quoted
 *   text of each enclosing level, so that text grows fast with the nesting
 *   depth. The text is only built up to the limit, so a refused key costs at
 *   most 64 KiB of text whatever its shape and however many aliases it
 *   expands. No realistic document that a person writes or a program
 *   generates gets near either limit.
 *   cyaml_serialize(), cyaml_serialize_flow(), cyaml_serialize_stream() and
 *   cyaml_clone() apply exactly the same nesting limit, because the tree a
 *   caller hands them doesn't have to come from cyaml_parse(): the caller
 *   can build it directly with cyaml_create_list() and
 *   cyaml_dictionary_set(). So every tree a parse gives serializes and
 *   clones, the text of every serialization parses back, and a serialize or
 *   clone of a tree that nests more than 500 containers returns NULL. None
 *   of these functions has a separate error-string channel, so the caller
 *   can't tell this NULL from an allocation failure. The parse, the
 *   serialize and the clone all walk the tree with an explicit worklist
 *   instead of recursion, so the call stack each one needs is a constant
 *   that doesn't grow with the nesting depth. Each of these limits bounds
 *   the work of a single call, not the stack that the caller must
 *   provide.
 * - Node count limit. One cyaml_parse() or cyaml_parse_n() call rejects a
 *   document that needs more DOM nodes than the larger of 4,000,000 and 4
 *   per byte of input. Of those, the nodes the parse makes by copying (see
 *   "Alias expansion limit" below) may number at most 4,000,000, whatever
 *   the input length. The parse error names the limit explicitly, it isn't
 *   an out-of-memory message, and the amount of free memory doesn't change
 *   it. The limit doesn't apply to a tree that the caller builds directly
 *   with cyaml_create_list() and cyaml_dictionary_set() outside a parse.
 * - Document memory limit. One cyaml_parse() or cyaml_parse_n() call also
 *   rejects a document whose DOM would use more than the larger of 256 MiB
 *   and 512 bytes per byte of input. The count covers the node structs,
 *   their backing containers, and the text of every scalar, every tag and
 *   every dictionary key, and here too the parse error names the limit
 *   instead of reporting out of memory. This limit covers what the node
 *   count alone can't: a node isn't a fixed amount of memory, and an alias
 *   deep-copies everything its anchor holds, so a small document whose
 *   bytes sit in a few large anchored scalars could expand without bound
 *   while its node count stays low. An ordinary document of any size stays
 *   well below both limits. Like the node limit it only applies to a parse,
 *   never to a tree that the caller builds directly through the public API.
 * - Alias expansion limit. The nodes and bytes that a parse makes by
 *   copying what it has already built count against a fixed limit of
 *   4,000,000 nodes and 256 MiB, which doesn't grow with the input. A copy
 *   is the node that an alias resolves to, a member that a merge key copies
 *   into its mapping, or the canonical text of a key that is a sequence or
 *   a mapping. An anchor by itself copies nothing: an anchored node of any
 *   size that no alias names costs exactly what the same node without the
 *   anchor costs, and only each alias of it pays for its own copy. These are
 *   the only ways a document can make more DOM than its text describes, so
 *   a nested-alias document hits this limit at the same point whether or
 *   not a long comment pads it. The copies also count against the two
 *   limits above, and the parse error names this limit.
 * - Integer range. CYAML_INTEGER is a signed 64-bit long long. A decimal
 *   integer literal that doesn't fit is a CYAML_FLOAT of the same
 *   magnitude, since a decimal literal is float text too. The hex and octal
 *   forms follow the strict core schema: "0x" followed by hex digits and
 *   "0o" followed by octal digits, with a lowercase 'x' or 'o' and no sign,
 *   so "-0x1", "+0x10" and "-0o7" are strings. An unsigned hex or octal
 *   literal above LLONG_MAX ("0x8000000000000000" and up) is a string too,
 *   because neither form has a float reading. A literal never silently
 *   becomes a CYAML_INTEGER that wrapped around or has the wrong sign. An
 *   explicit !!int or !!float tag refuses every such text as a parse error.
 * - Long keys in the output. An implicit key ("key: value") holds at most
 *   1024 characters under YAML 1.2, and PyYAML and libyaml refuse a longer
 *   one. So cyaml_serialize() writes a key whose rendered text is longer
 *   than 1024 bytes in the explicit form ("? key", then ": value" on the
 *   next line), and cyaml_serialize_flow() writes it as an explicit
 *   "{? key: value}" entry. The parser reads both forms back as the same
 *   entry.
 * - Numbers and the locale. The parser and both serializers always use '.'
 *   as the decimal point of a float, whatever LC_NUMERIC the process or the
 *   calling thread uses. For the length of one cyaml_parse*() or
 *   cyaml_serialize*() call the calling thread runs in the "C" locale
 *   through uselocale(), which doesn't change any other thread, and the
 *   thread's locale is restored before the call returns. A custom allocator
 *   that the call invokes runs inside that window.
 *
 * ### Custom memory management
 *
 * Every factory function and the parser take a ccol_memmgmt_procs_t *mp
 * parameter; pass NULL for the default malloc, free, calloc and realloc.
 * Each node stores its own allocator, and all the nodes of one tree must
 * carry the same allocator.
 *
 * The library keeps its own copy of the procs struct and the nodes point at
 * that copy, never at the caller's struct. So the caller may build the
 * struct on the stack of a helper function, or release it, while trees
 * built with it are still in use, but the functions it names must stay
 * callable until the last node built with them is destroyed. The copies
 * live as long as the process, one for each distinct set of four functions,
 * so nodes built from two structs with the same four functions share one
 * copy. A process can use at most 64 distinct sets, and a factory function
 * or a parse that would need a 65th fails (NULL).
 *
 * Each thread keeps a free-list pool of up to 512 nodes, which makes the
 * default-allocator case faster. A node with a custom allocator doesn't use
 * the pool, and the pool is drained when the thread stops.
 *
 * ### Path syntax (cyaml_get / cyaml_set)
 *
 * The syntax is the same as in cjson: a path has dot-separated components,
 * for example "server.hosts.#0.port". A component that starts with '#'
 * followed by digits indexes into a list when the current node is a
 * CYAML_LIST; in every other case the whole component, '#' included, is
 * taken as a literal dictionary key. An index with any number of digits is
 * well formed, so an index that the list doesn't reach is an absent
 * component however large it is: cyaml_get() gives NULL, and cyaml_set()
 * and cyaml_delete() give ccol_key_not_found.
 *
 * A path string has two escape sequences, which let you address a key that
 * holds a literal '.' or '\'. '\.' resolves to a literal '.' in the key
 * instead of a path separator, and '\\' resolves to a literal '\'. A '\'
 * before any other character passes through unchanged. For example,
 * cyaml_get(doc, "a\\.b") addresses a key with the literal name "a.b", and
 * cyaml_get(doc, "a\\.b.c\\.d") addresses the key "c.d" one level under the
 * key "a.b".
 *
 * ### Ownership
 *
 * - cyaml_create_*(), cyaml_parse*() and cyaml_clone() give trees that the
 *   caller owns in full.
 * - On success cyaml_list_push() and cyaml_dictionary_set() move ownership
 *   of the child to the parent, so don't free the child after such a call.
 *   Each return code carries one rule about the child: ccol_invalid_args
 *   always means that the arguments were rejected and the child wasn't
 *   touched at all, so the caller still owns it, while every other failure
 *   takes ownership of the child and deep-frees it. The child must not
 *   already be attached to a list or dictionary parent. A fresh node is
 *   fine, and so is a fresh cyaml_clone(), but a borrowed reference from
 *   cyaml_get(), cyaml_list_get() or cyaml_dictionary_get() is not: an
 *   already-attached node would get two owners if it were accepted, and
 *   each owner frees it when its own parent is destroyed. The doc comment of
 *   each function lists every rejection and the one exception that does
 *   nothing.
 * - cyaml_get() gives a NON-OWNING reference, which stays valid until the
 *   caller changes or destroys the tree.
 * - cyaml_destroy() frees the whole subtree and sets the handle to NULL. It
 *   tears the tree down with a loop instead of recursion, so freeing a tree
 *   of any depth never grows the native call stack.
 * - Every tree that cyaml_parse*() builds is acyclic by construction. A tree
 *   that the caller builds or changes directly with cyaml_list_push() or
 *   cyaml_dictionary_set() must stay acyclic too, because neither function
 *   checks for a cycle, such as a node that goes into its own subtree.
 *   Passing a cyclic tree to cyaml_destroy(), cyaml_clone() or
 *   cyaml_serialize*() is undefined behavior.
 *
 * ### Serialization
 *
 * cyaml_serialize() gives block-style YAML, and cyaml_serialize_flow()
 * gives compact flow-style YAML on one line. The returned buffer is
 * allocated with the allocator of the root node; free it with
 * cyaml_serialize_free(), or with cyaml_serialize_free_mp() and the same mp
 * that you used at parse or create time (NULL for the default allocator).
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
 * Unlike JSON, YAML has explicit .inf and .nan, so a non-finite value is
 * stored and serialized as .inf, -.inf or .nan.
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
 * @param val  Null-terminated, well-formed UTF-8 string, which the node
 *             copies. NULL gives a CYAML_NULL node. Every Unicode character
 *             is accepted, but a string that isn't well-formed UTF-8 is
 *             refused.
 * @param mp   Custom allocator, or NULL for default.
 * @return The new node, or NULL when val isn't well-formed UTF-8 or an
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
 * header). Any other input is refused as a parse error whose message gives
 * the line and column of the first offending character.
 *
 * @param yaml_str  Input text (must be null-terminated).
 * @param err_str   Out-parameter for the error message of a failed parse. On
 *                  success *err_str is set to NULL, and on failure it points
 *                  at a message that the LIBRARY owns. Never free that
 *                  string, not with cyaml_serialize_free_mp() either, since
 *                  it isn't an allocation; the err out-parameter of every
 *                  other module in this library has the same rule. The text
 *                  stays valid until the next FAILING parse on the same
 *                  thread, the lifetime that strerror(3) and dlerror(3)
 *                  give, and the storage is per-thread, so two threads that
 *                  parse at the same time never overwrite each other's
 *                  message. Copy the text if you need it longer than that.
 *                  When the thread can't keep a message buffer (memory runs
 *                  out, or the process has no thread-specific key left for
 *                  it), the message is a fixed text that names no reason.
 *                  Success or failure always shows in the return value and
 *                  never in *err_str. Pass NULL to ignore the error details.
 *                  The message is printable ASCII whatever the input holds:
 *                  where it quotes input text (a scalar, a tag, an anchor or
 *                  alias name, a character) it writes a printable ASCII byte
 *                  as it is and any other byte as <0xNN>, and it cuts a long
 *                  quoted text short at a whole byte with "...". A character
 *                  that the input may not hold at all is named as 0xNN.
 * @param mp        Custom allocator for all the nodes of the tree this
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
 * Exactly @p len bytes are read, and the character rules of cyaml_parse_mp()
 * apply to every one of them: a null byte, any other refused character, or
 * a UTF-8 sequence that @p len cuts short is a parse error. A byte past
 * @p len is never read.
 *
 * @param yaml_str  Input buffer.
 * @param len       Number of bytes to parse.
 * @param err_str   Out-parameter for the error message. It works as in
 *                  cyaml_parse_mp and has the same ownership rule: the
 *                  library owns the string and the caller never frees it.
 *                  The message is printable ASCII whatever the len bytes
 *                  hold, as cyaml_parse_mp describes. Pass NULL to ignore
 *                  it.
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
 * A scalar is written in plain form, which reads best, when that form is
 * safe, and double-quoted otherwise. The plain form is only used when YAML
 * 1.1 readers such as PyYAML and libyaml read it as the same string too, so
 * a string holding a tab, a '?', or one of the YAML 1.1 line breaks NEL
 * (U+0085), LS (U+2028) and PS (U+2029), and the string "=", go out
 * double-quoted, with those line breaks written as their \N, \L and \P
 * escapes. A list uses the "- item" block syntax and a dictionary the
 * "key: value" block syntax, with members in insertion order (see
 * "Implementation-defined behavior" > "Dictionary key order" above). A float
 * always has a '.' in its mantissa ("1.0", "1.0e+20") so that a YAML 1.1
 * reader also reads it as a float, and its digits are the shortest of 15,
 * 16 or 17 significant digits that read back as the same double; a
 * subnormal double, which has fewer significant bits, takes the shortest of
 * 1 to 17 digits, so 5e-324 goes out as "5.0e-324".
 *
 * The returned buffer is allocated with the allocator of the root node. Free
 * it with cyaml_serialize_free(), or with cyaml_serialize_free_mp() and the
 * same mp that created the tree.
 *
 * The tree is walked with an explicit worklist instead of recursion, so the
 * call stack this function needs is a constant that doesn't grow with the
 * nesting depth.
 *
 * @param node  Root of the tree or subtree to serialize. A NULL node
 *              serializes the same way as a CYAML_NULL node ("~\n"), which
 *              is a successful, non-NULL return and not a failure.
 * @return A null-terminated YAML string on the heap. NULL when memory runs
 *         out, and also when node nests more than 500 containers (see
 *         "Implementation-defined behavior" > "Nesting limits" above). This
 *         function has no separate error-string channel, so the caller
 *         can't tell the two causes apart.
 */
char *cyaml_serialize(cyaml node);

/**
 * @brief Serialize a DOM tree to compact flow-style YAML.
 *
 * The output looks like JSON: a list becomes [a, b, c] and a dictionary
 * becomes {key: value}, which is handy for a compact one-line text.
 * Dictionary members and the spelling of a float follow the same rules as in
 * cyaml_serialize() above, and for the same reason this function also walks
 * the tree with an explicit worklist instead of recursion.
 *
 * @param node  Root of the tree or subtree to serialize. A NULL node
 *              serializes the same way as a CYAML_NULL node ("~"), which is
 *              a successful, non-NULL return and not a failure.
 * @return A null-terminated string on the heap. NULL when memory runs out,
 *         and also when node nests more than 500 containers; see the same
 *         note on cyaml_serialize() above.
 */
char *cyaml_serialize_flow(cyaml node);

/**
 * @brief Serialize a list of documents as a multi-document YAML stream.
 *
 * This is the counterpart of parsing a stream. For a stream with more than
 * one document cyaml_parse() gives a CYAML_LIST with one element per
 * document, and this function writes such a list back as a stream: each
 * element becomes its own document, which starts with a "---" line and holds
 * the block-style YAML that cyaml_serialize() writes for that element. So
 * parsing the result for a list of two or more documents gives back an equal
 * list (parsing a one-document stream gives its root), and a stream of
 * Kubernetes manifests, for example, can be parsed, edited and written back
 * for a tool that reads it one document at a time.
 *
 * - An empty list gives an empty stream, the empty string "". Parsing an
 *   empty string gives a CYAML_NULL and not an empty list, because a stream
 *   with no documents and an empty document read the same.
 * - A node that isn't a list, a NULL node included, is written as a stream
 *   of one document, so the root of a one-document parse writes back as
 *   that same document. A list that is the root of a single document can't
 *   be told apart from a stream of documents, neither in the parse nor
 *   here; write such a document with cyaml_serialize().
 * - A tag of the list itself isn't written, since a stream has no node that
 *   could carry it. A list that a parse of a stream gives never carries one.
 * - Each document stands alone. The output holds no directive and no
 *   anchor: a tag goes out in the same form as in cyaml_serialize(), which
 *   needs no %TAG directive, and an alias of the parsed input is already an
 *   independent copy in the tree.
 *
 * The buffer is allocated with the allocator of @p node. Free it with
 * cyaml_serialize_free() or cyaml_serialize_free_mp().
 *
 * @param node  The list whose elements are the documents, or a single
 *              document root.
 * @return A null-terminated YAML string on the heap. NULL when memory runs
 *         out, and also when node nests more than 500 containers, the list
 *         counted, as in cyaml_serialize(): each element of a list may nest
 *         at most 499 containers, exactly like each document of a parsed
 *         multi-document stream, and a node that isn't a list may nest 500.
 */
char *cyaml_serialize_stream(cyaml node);

/**
 * @brief Free a string returned by cyaml_serialize(),
 *        cyaml_serialize_flow() or cyaml_serialize_stream().
 *
 * @param s   String to free. It can be NULL.
 * @param mp  The allocator that was used to create the tree. Pass NULL when
 *            the tree used the default allocator.
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
 * @param node  It can be NULL, and then CYAML_NULL is returned.
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
 * Compare one of them against what cyaml_node_tag() returns, or pass one to
 * cyaml_node_set_tag(), so you don't have to type the URI strings by hand.
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
 * the fully resolved tag string here, whether the tag was "!!str", a custom
 * "!foo", a shorthand that %TAG resolves, or a verbatim tag. A node with no
 * tag returns NULL, and so does a node whose type only comes from implicit
 * core-schema resolution. The tag is one of the seven CYAML_TAG_* constants
 * above or a custom tag string, depending on what the source document, or
 * cyaml_node_set_tag(), gave.
 *
 * A dictionary key can never carry a tag, because the keys of this DOM are
 * plain strings and not nodes (see the doc comment of
 * cyaml_dictionary_get()). A tag on a key in the document types that key
 * exactly as it types a value, and the dictionary stores the canonical text
 * of the typed key, but there's nowhere to keep the tag itself.
 *
 * The node owns the tag string, so don't free it. The pointer is borrowed
 * and stays valid until the node's tag or value is set again, or the node is
 * destroyed. Any of these ends it: a cyaml_node_set_tag() on the node, a
 * cyaml_set() that reaches the node (a new value drops a core tag that no
 * longer matches it), a cyaml_list_remove(), cyaml_dictionary_remove() or
 * cyaml_delete() of the node, a cyaml_dictionary_set() that replaces it, and
 * a destroy of its tree. Copy the text if you need it longer.
 *
 * @param node  It can be NULL, and then NULL is returned.
 */
const char *cyaml_node_tag(cyaml node);

/**
 * @brief Attach or replace a node's tag, or clear it.
 *
 * This only sets metadata and never changes the node's type or value. A
 * core-schema tag (one of the seven CYAML_TAG_* constants) names exactly one
 * node type: CYAML_TAG_NULL names CYAML_NULL, CYAML_TAG_BOOL names
 * CYAML_BOOL, CYAML_TAG_INT names CYAML_INTEGER, CYAML_TAG_FLOAT names
 * CYAML_FLOAT, CYAML_TAG_STR names CYAML_STRING, CYAML_TAG_SEQ names
 * CYAML_LIST and CYAML_TAG_MAP names CYAML_DICTIONARY. A core-schema tag on a
 * node of any other type is refused with ccol_invalid_args, because
 * cyaml_parse() either rejects that pairing or reads it back as a different
 * type; a caller that wants a string node must create one with
 * cyaml_create_string() and then tag it. A custom tag is valid on a node of
 * any type.
 *
 * A custom tag set here survives a later cyaml_set() on the same node, while
 * a core-schema tag only survives as long as it names the node's type:
 * cyaml_set() drops a core-schema tag that doesn't match the new value. For
 * example, a node parsed from "port: !!int 8080" loses its tag when
 * cyaml_set() writes the string "auto" to it, and keeps it when cyaml_set()
 * writes the integer 9090.
 *
 * The empty string isn't a valid tag and is rejected with ccol_invalid_args:
 * the serializer writes a custom tag in its verbatim "!<...>" form, and
 * cyaml_parse() doesn't accept "!<>", so an empty tag would give a document
 * that this library can't parse again. Pass NULL to clear a tag. "!" alone
 * is the non-specific tag, which no node carries, and it is rejected with
 * ccol_invalid_args as well. A tag must be well-formed UTF-8, and one that
 * isn't is refused with ccol_invalid_args. Every other string is accepted,
 * including one with a character that can't appear literally inside
 * "!<...>"; the "Tags" section at the top of this header describes how such
 * a character survives a round trip.
 *
 * @param node  Target node of any type. For NULL the function reports
 *              ccol_invalid_args and does nothing else.
 * @param tag   New tag string, which the node copies. It must not be the
 *              empty string. NULL clears the node's current tag.
 * @return ccol_success. ccol_invalid_args when node is NULL, when tag is the
 *         empty string or "!", when tag isn't well-formed UTF-8, and when tag
 *         is a core-schema tag that doesn't name the type of node, and
 *         ccol_not_enough_memory on an allocation failure.
 *         On every failure the node's current tag is left untouched.
 */
ccol_retval_t cyaml_node_set_tag(cyaml node, const char *tag);

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/**
 * @brief Return the boolean value.
 * It calls _ccol_fatal_err() when the type of the node is not CYAML_BOOL.
 */
bool cyaml_bool_val(cyaml node);

/**
 * @brief Return the integer value.
 * It calls _ccol_fatal_err() when the type of the node is not CYAML_INTEGER.
 */
long long cyaml_int_val(cyaml node);

/**
 * @brief Return the floating-point value.
 * It calls _ccol_fatal_err() when the type of the node is not CYAML_FLOAT.
 */
double cyaml_double_val(cyaml node);

/**
 * @brief Return the string value. The node owns it, so don't free it.
 * It calls _ccol_fatal_err() when the type of the node is not CYAML_STRING.
 *
 * The pointer is borrowed and stays valid until the node gets a new value or
 * is destroyed. Any of these ends it: a cyaml_set() that reaches the node, a
 * cyaml_list_remove(), cyaml_dictionary_remove() or cyaml_delete() of the
 * node, a cyaml_dictionary_set() that replaces it, and a destroy of its
 * tree. Copy the text if you need it longer.
 */
const char *cyaml_str_val(cyaml node);

/**
 * @brief Return the element count of a list node.
 * It calls _ccol_fatal_err() when the type of the node is not CYAML_LIST.
 */
size_t cyaml_list_len(cyaml node);

/**
 * @brief Return the key count of a dictionary node.
 * It calls _ccol_fatal_err() when the type of the node is not
 * CYAML_DICTIONARY.
 */
size_t cyaml_dictionary_size(cyaml node);

/* ========================================================================== */
/*                          LIST / DICTIONARY MANIPULATION                    */
/* ========================================================================== */

/**
 * @brief Append a child node to a list.
 *
 * @p seq takes ownership of @p child, so don't free @p child after this
 * call.
 *
 * @p child must not already be attached to a list or dictionary parent, and
 * it must not be @p seq itself. Each of these two cases is rejected with
 * ccol_invalid_args and @p child is left completely untouched, owned by
 * whatever it is already attached to. If it were accepted the node would
 * have two owners, each of which frees it when its own parent is destroyed.
 * A node you have just created is always fine, and so is a fresh
 * cyaml_clone(), but a borrowed reference from cyaml_get(),
 * cyaml_list_get() or cyaml_dictionary_get() never is, because each of
 * those names a node that already has a parent.
 *
 * A NULL @p child, a NULL @p seq and an @p seq that isn't a CYAML_LIST are
 * rejected the same way: ccol_invalid_args, with @p child untouched and
 * owned by the caller.
 *
 * On every other failure (out of memory and ccol_container_full) ownership
 * moves anyway, and @p child is deep-freed before the function returns. So
 * ccol_invalid_args always means that the caller owns @p child, and every
 * other failure code means that it doesn't.
 *
 * @p child must also not already contain @p seq, directly or through another
 * node. This isn't checked, and a cycle made that way turns a later
 * cyaml_destroy(), cyaml_clone() or cyaml_serialize*() on the tree into
 * undefined behavior. The attachment rule above already excludes this for
 * every node that has a parent, so what stays unchecked is an ancestor with
 * no parent: the root of the tree that @p seq belongs to.
 *
 * @param seq    Target list node (it must be CYAML_LIST).
 * @param child  Child to append.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_container_full. ccol_container_full means the list holds the
 *         largest element count it can represent, which no real program
 *         reaches.
 */
ccol_retval_t cyaml_list_push(cyaml seq, cyaml child);

/**
 * @brief Access an element of a list by index (non-owning).
 * @return The child node, or NULL when index is out of bounds or the type
 *         doesn't match.
 */
cyaml cyaml_list_get(cyaml seq, size_t index);

/**
 * @brief Set (insert or replace) a key in a dictionary.
 *
 * @p map takes ownership of @p child, so don't free @p child after this
 * call.
 *
 * Every ccol_invalid_args leaves @p child untouched and owned by the caller.
 * That covers a NULL @p child, a NULL @p map, a NULL @p key, a @p key that
 * isn't well-formed UTF-8, an @p map that isn't a CYAML_DICTIONARY, and the
 * two attachment rejections below. On every other failure (out of memory or
 * ccol_container_full) ownership moves anyway, and @p child is deep-freed
 * before the function returns.
 *
 * When the key is already present, the previous child is deep-freed and the
 * new child goes into the same slot. Nothing between the two steps can fail,
 * so the slot never names freed memory once the call returns, and the two
 * children are never in memory at the same time. The key keeps its place in
 * the member order of the dictionary, while a new key goes after every member
 * the dictionary already holds; see cyaml_dictionary_iter.
 *
 * @p child must not already be attached to a list or dictionary parent, and
 * it must not be @p map itself. Each of these two cases is rejected with
 * ccol_invalid_args and @p child is left completely untouched; the doc
 * comment of cyaml_list_push() explains why. The one exception is setting
 * @p key to the value it already holds: the borrowed reference that this
 * same slot owns is accepted, nothing happens, and ccol_success comes back.
 *
 * @p child must also not already contain @p map, directly or through another
 * node. This isn't checked, and a cycle made that way turns a later
 * cyaml_destroy(), cyaml_clone() or cyaml_serialize*() on the tree into
 * undefined behavior.
 *
 * @param map    Target dictionary node (it must be CYAML_DICTIONARY).
 * @param key    Null-terminated, well-formed UTF-8 key string, which the
 *               dictionary copies.
 * @param child  Value node.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_container_full. ccol_container_full means the dictionary holds
 *         the largest element count it can represent, which no real program
 *         reaches.
 */
ccol_retval_t cyaml_dictionary_set(cyaml map, const char *key, cyaml child);

/**
 * @brief Look up a key in a dictionary (non-owning).
 * @return The child node, or NULL when the key is absent or the type doesn't
 *         match.
 */
cyaml cyaml_dictionary_get(cyaml map, const char *key);

/**
 * @brief Remove and deep-free the element at position index from a list.
 *
 * Every element after index shifts left by one position, and the removed
 * subtree is freed recursively.
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
 * The removed subtree is freed recursively.
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
 * The caller owns the struct, usually on its own stack, and the iteration
 * allocates nothing and can't fail. Fill it with cyaml_dictionary_first() and
 * step it with cyaml_dictionary_next(). Read only @c key and @c value; the
 * fields whose names start with an underscore are private.
 *
 * The order is the order in which each key first entered the dictionary,
 * which is the order of the members in the parsed text, or the order of the
 * cyaml_dictionary_set() and cyaml_set() calls that added them. Replacing the
 * value of a key keeps its place, so a key that a document repeats keeps the
 * place of its first occurrence and the value of its last, while a removed
 * key leaves the order. The members that a merge key ("<<") brings in take
 * the place of the first merge key that brings their key, source by source
 * in the order the merge key lists them, each source in its own member
 * order; a key the mapping already holds keeps its own place and value. The
 * serializers and cyaml_clone() follow the same order.
 *
 * The cursor holds the member that the next step reads, which is the
 * successor of the member the last successful call gave, and it stays valid
 * across every change of the dictionary that leaves that successor in place:
 *
 *   - replacing the value of any member, the current one included, through
 *     cyaml_dictionary_set() or cyaml_set(). The member keeps its place, so
 *     the iteration goes on in the same order;
 *   - removing the current member, or any member other than the successor;
 *   - inserting a new key. The new member goes after every other member,
 *     and whether this iteration reaches it is unspecified.
 *
 * Removing the successor, through cyaml_dictionary_remove(), cyaml_delete()
 * or any other call, invalidates the cursor, and so does destroying the
 * dictionary; start again with cyaml_dictionary_first() after such a change.
 *
 * The @c key and @c value fields describe the current member as it was when
 * the cursor stepped onto it. Replacing the value of the current member
 * through cyaml_dictionary_set() destroys the old value, so @c value then
 * names freed memory; read the new value with cyaml_dictionary_get() and
 * @c key. cyaml_set() on the other hand updates the existing node in place,
 * so @c value stays the same node.
 */
typedef struct cyaml_dictionary_iter {
  /** The key of the current member, valid until that member is removed or
   *  the dictionary is destroyed. NULL once the iteration is over. */
  const char *key;
  /** The value of the current member, a borrowed reference with the rules
   *  of cyaml_dictionary_get(). NULL once the iteration is over. */
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
 * @return true when @p it stands on the first member. false when the
 *         dictionary is empty, when @p dict is NULL, or when @p dict is not
 *         a CYAML_DICTIONARY, and then @c key and @c value are set to NULL.
 *         false also when @p it is NULL.
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
 * @return true when @p it stands on the next member. false when the previous
 *         call gave the last member or an earlier call has already returned
 *         false, and then @c key and @c value are set to NULL. false also
 *         when @p it is NULL.
 */
bool cyaml_dictionary_next(cyaml_dictionary_iter *it);

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/**
 * @brief Return a fully independent deep copy of a DOM subtree.
 *
 * The clone uses the same allocator as the source tree, carries over the
 * tag of every node that has one unchanged (see cyaml_node_tag()), and keeps
 * the source's member order in every dictionary (see
 * cyaml_dictionary_iter). The source is walked with an explicit worklist
 * instead of recursion, so the call stack this function needs is a constant
 * that doesn't grow with the nesting depth.
 *
 * @param node  Root of the tree or subtree to clone. A NULL node gives NULL,
 *              since there's no tree to copy; that's different from
 *              cyaml_serialize() and cyaml_serialize_flow(), which treat a
 *              NULL node as a successful CYAML_NULL value.
 * @return The new root node, which the caller owns. NULL when node is itself
 *         NULL, when memory runs out, and when node nests more than 500
 *         containers (see "Implementation-defined behavior" > "Nesting
 *         limits" above). This function has no separate error-string
 *         channel, so the caller can't tell these last two causes apart.
 */
cyaml cyaml_clone(cyaml node);

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized cyaml variable.
 *
 * It's here so that the naming convention matches the other container
 * modules.
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
 * Calling it on a variable that holds NULL does nothing. @p node must be an
 * assignable lvalue, so you can't pass a plain NULL literal, because the
 * macro sets @p node to NULL after the free. Use ___cyaml_destroy() directly
 * when you don't have an lvalue.
 *
 * @note The macro evaluates node exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define cyaml_destroy(node)      \
  _ccol_cyaml_destroy_impl(node, \
                           _ccol_uniq(__ccol_cyaml_destroy_slot, __COUNTER__))

/* Internal: the body of cyaml_destroy. slot is a name from _ccol_uniq(), so
 * the macro can nest inside the argument of another destroy macro and stay
 * -Wshadow clean, and the argument is evaluated exactly once. */
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
 * A NULL or empty path returns root itself, since a path with no component
 * addresses the root. That matches cyaml_set() and cyaml_delete(), which
 * read a path that is only a leaf, for example "key", as a child of root, so
 * such a path needs no leading part at all.
 *
 * @return The target node. NULL when root is NULL, when a path component is
 *         missing or has the wrong type, and when the path holds an empty
 *         component (nothing between two dots, a leading dot, or a trailing
 *         dot). A path of 256 bytes or more is first copied with the
 *         allocator of root, and NULL also comes back when that copy fails;
 *         a shorter path needs no allocation.
 */
cyaml _cyaml_get(cyaml root, const char *path);

/**
 * @brief Remove and deep-free the node addressed by a path (back-end).
 *
 * Prefer the cyaml_delete() macro.
 *
 * This walks to the parent of the addressed node and then calls
 * cyaml_dictionary_remove() or cyaml_list_remove(), whichever fits the
 * parent. The path uses the same dot-separated syntax as _cyaml_get and
 * _cyaml_set_typed, with the same escape sequences.
 *
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL or empty path, a wrong parent type,
 *         a bad "#N" index syntax, an empty path component (nothing between
 *         two dots, a leading dot, or a trailing dot), or a path component
 *         that isn't well-formed UTF-8. Any path component can cause this,
 *         not only the leaf, and it wins over an absent component earlier in
 *         the path.
 *         ccol_key_not_found only when a syntactically correct path
 *         component (a dictionary key or a "#N" index) is absent, at any
 *         position, and also when a syntactically correct list index is out
 *         of range.
 *         ccol_not_enough_memory on an allocation failure.
 */
ccol_retval_t _cyaml_delete(cyaml root, const char *path);

/**
 * @brief Write a typed scalar value to the leaf addressed by a path (back-end).
 *
 * Prefer the cyaml_set() macro.
 *
 * For a dictionary leaf the key is created when it is absent, but the parent
 * dictionary must already exist. For a list leaf ("#N") the index must
 * already be in range, because a list never grows to fit an index
 * that the caller picks. In both cases the value at the leaf is replaced and
 * the old value, whole subtrees included, is deep-freed.
 *
 * @param root             Root of the DOM tree.
 * @param path             Dot-separated path string.
 * @param type             YAML type of the new value.
 * @param raw              Pointer to the raw C value (passed as void *). It
 *                         may be NULL only for CYAML_NULL; a NULL raw for any
 *                         other type is rejected with ccol_invalid_args.
 * @param raw_size         sizeof() of the original C expression, which must
 *                         match the object that @p raw names: 1, 2, 4 or 8
 *                         for CYAML_INTEGER, sizeof(float) or sizeof(double)
 *                         for CYAML_FLOAT, sizeof(bool) for CYAML_BOOL, and
 *                         sizeof(const char *) for CYAML_STRING, where
 *                         @p raw names a pointer to the string and not the
 *                         string itself. Every other size is rejected with
 *                         ccol_invalid_args before the value is read, so the
 *                         target leaf is left untouched.
 * @param is_signed        Whether the integer source type is signed.
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL or empty path, a wrong parent type,
 *         a bad "#N" index syntax, an empty path component, a path component
 *         that isn't well-formed UTF-8, or a CYAML_STRING value that isn't
 *         well-formed UTF-8, each of which leaves the tree unchanged. An
 *         empty component is nothing between two dots, a leading dot, or a
 *         trailing dot, and any path component can cause this, not only the
 *         leaf, even after an absent component.
 *         ccol_key_not_found when a list index is syntactically correct but
 *         out of range, at the leaf or above it, and also when a dictionary
 *         key above the leaf is absent, since that key is needed to resolve
 *         the parent path. An absent dictionary key at the leaf itself is not
 *         an error, because it gets created.
 *         ccol_not_enough_memory on an allocation failure.
 *         ccol_container_full when a new leaf dictionary key would take the
 *         parent past the largest element count it can represent, which no
 *         real program reaches.
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
 * cyaml_set() documents the types it accepts: bool, any integer type, float,
 * double, char *, const char *, and a bare NULL (a void * value that isn't
 * NULL is refused at run time). _cyaml_type_of() gives this sentinel for
 * every other type, such as long double or a struct, i.e. anything that the
 * default association of the _Generic below catches.
 *
 * A bare `NULL` literal has the type void *, and on purpose it gets its own
 * explicit association below instead of this default, as the C23 nullptr
 * does through _CCOL_NULLPTR_ASSOC, because passing NULL is documented,
 * intentional use and not a caller mistake; the doc comment of cyaml_set
 * says so.
 *
 * The sentinel is on purpose NOT a real cyaml_node_type_t enumerator: no
 * node stores it, cyaml_type() never returns it, and it only travels through
 * the `type` argument of _cyaml_set_typed(). That function already has a
 * switch that checks the type, scalar_payload_check() in cyaml.c, which
 * rejects every value outside {CYAML_NULL, CYAML_BOOL, CYAML_INTEGER,
 * CYAML_FLOAT, CYAML_STRING} with ccol_invalid_args before it touches the
 * target node. Because the value differs from every real enumerator, that
 * guard fires for an unsupported type, whereas a default of CYAML_NULL is on
 * the accept list and would pass the guard silently.
 */
#define _CYAML_TYPE_UNSUPPORTED ((cyaml_node_type_t)0x7f)

/**
 * @brief Map the compile-time type of a C expression to cyaml_node_type_t.
 */
#if defined(_CCOL_CLANG_GENERIC_ASSOC_WARNING)
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

/* Compile-time constant, true when the platform's plain 'char' is signed. */
#if defined(__CHAR_UNSIGNED__)
#define _CYAML_CHAR_SIGNED false
#else
#define _CYAML_CHAR_SIGNED true
#endif

#if defined(_CCOL_CLANG_GENERIC_ASSOC_WARNING)
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
 * @param path  Dot-separated path, a string literal or a char *.
 * @return A non-owning cyaml handle, or NULL when the path doesn't exist.
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
 * The value can be a bool, any integer type, float, double, char *,
 * const char *, a char array or a string literal. A NULL, or the C23
 * nullptr, sets the leaf to CYAML_NULL. Before C23 the macro `true` is the
 * int 1, so cyaml_set(doc, k, true) stores the CYAML_INTEGER 1 and you need
 * to write (bool)true to get a CYAML_BOOL; in C23 `true` has the type bool
 * and needs no cast. An integer becomes a CYAML_INTEGER, except an unsigned
 * value above LLONG_MAX, which no long long can hold: that becomes a
 * CYAML_FLOAT with the nearest double, the same node cyaml_parse() gives for
 * its decimal literal. A void * value that isn't NULL is refused with
 * ccol_invalid_args, because only a NULL counts as an intentional null. A
 * bit-field doesn't compile as val because C forbids __typeof__ on one, so
 * cast it to its integer type or to bool.
 *
 * A string value, and a leaf component that addresses a dictionary key, must
 * be well-formed UTF-8; for one that isn't, the macro returns
 * ccol_invalid_args and changes nothing.
 *
 * For a dictionary leaf the key is created when it is absent, but the parent
 * dictionary must already exist. For a list leaf ("#N") the index must
 * already be in range, because a list never grows to fit an index
 * that the caller picks; the doc comment of _cyaml_set_typed() gives the
 * exact split between ccol_key_not_found and ccol_invalid_args that follows
 * from this. When the leaf already exists its type always changes. A custom
 * tag on that leaf survives, while a core-schema tag only survives as long
 * as it names the type of the new value and is dropped otherwise (see
 * cyaml_node_set_tag()).
 *
 * @param root  Root cyaml handle.
 * @param path  Dot-separated path string.
 * @param val   C value, whose type _Generic works out at compile time. A
 *              string literal, a `char[N]` array and a pointer to char all
 *              give a CYAML_STRING, whatever their qualifiers: val is copied
 *              into a local of its own type after array-to-pointer
 *              conversion, with top-level qualifiers removed, so an array
 *              arrives as a pointer to its first character.
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
 * which lets cyaml_set() nest under -Wshadow inside its own argument and
 * inside the argument of any other public macro. */
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
 * The macro walks to the parent of the addressed node, then removes the
 * child and frees it recursively. Under a dictionary parent the path
 * addresses the leaf by key, and under a list parent the leaf must be a
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
