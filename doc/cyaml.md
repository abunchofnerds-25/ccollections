# cyaml: read, change and write YAML

`cyaml` parses YAML 1.2 into a tree of nodes that you can read and change;
`cyaml` then writes the tree back as YAML. If you know [cjson](cjson.md), you
already know most of `cyaml`: the node types, the path macros, the ownership
rules and the dictionary cursor are the same, with a `cyaml_` prefix.

Use `cyaml` for files that people write by hand, such as service
configuration, deployment manifests, CI pipelines and test fixtures. `cyaml`
supports the YAML features that these files usually rely on:

- block style and flow style,
- comments,
- strings on more than one line,
- anchors and aliases,
- merge keys (`<<`),
- tags,
- streams of more than one document.

Use a different tool in two cases:

- You must process a YAML stream that is much larger than memory while it
  arrives.
- You need YAML 1.1 behavior, where `yes` and `on` are booleans. `cyaml`
  follows YAML 1.2, in which `yes` and `on` are strings.

```c
#include <ccollections/cyaml.h>
```

## A first example

```c
#include <stdbool.h>
#include <stdio.h>
#include <ccollections/cyaml.h>

int main(void) {
    const char *text =
        "# service settings\n"
        "name: billing\n"
        "port: 8080\n"
        "hosts:\n"
        "  - db1.internal\n"
        "  - db2.internal\n";

    char *err = NULL;
    cyaml doc = cyaml_parse(text, &err);
    if (!doc) {
        fprintf(stderr, "parse error: %s\n", err); /* do not free err */
        return 1;
    }

    printf("name:  %s\n", cyaml_str_val(cyaml_get(doc, "name")));
    printf("port:  %lld\n", cyaml_int_val(cyaml_get(doc, "port")));
    printf("host1: %s\n", cyaml_str_val(cyaml_get(doc, "hosts.#1")));

    cyaml_set(doc, "port", 9090);
    cyaml_set(doc, "tls", (bool)true);

    char *out = cyaml_serialize(doc);   /* NULL when there is not enough memory */
    if (out)
        fputs(out, stdout);
    cyaml_serialize_free(out);

    cyaml_destroy(doc);
    return 0;
}
```

Build the example with `-std=gnu11` and link it with `-lccollections`:

```sh
gcc -std=gnu11 first.c -lccollections -o first
```

The program prints:

```
name:  billing
port:  8080
host1: db2.internal
name: billing
port: 9090
hosts:
  - db1.internal
  - db2.internal
tls: true
```

The output has no comment, because comments are not part of the tree. The keys keep their source order, and the new key comes last.

## How YAML values become nodes

Each value is a `cyaml` handle, and `cyaml_type()` tells you which type it
holds: `CYAML_NULL`, `CYAML_BOOL`, `CYAML_INTEGER` (a `long long`),
`CYAML_FLOAT` (a `double`), `CYAML_STRING`, `CYAML_LIST` or
`CYAML_DICTIONARY`. You read a scalar with `cyaml_bool_val`, `cyaml_int_val`,
`cyaml_double_val` or `cyaml_str_val`.

A quoted scalar is always a string, while the type of an unquoted scalar
comes from the YAML 1.2 core schema:

| You write | You get |
|---|---|
| `~`, `null`, `Null`, `NULL`, or nothing | `CYAML_NULL` |
| `true`, `True`, `TRUE`, `false`, ... | `CYAML_BOOL` |
| `42`, `-7`, `0x2A`, `0o52` | `CYAML_INTEGER` |
| `3.14`, `1e3`, `.inf`, `-.inf`, `.nan` | `CYAML_FLOAT` |
| all other text, for example `yes`, `on`, `1.2.3` | `CYAML_STRING` |

So when a value must stay text, put quotes around it: `version: '1.10'` is
the string `1.10`, but `version: 1.10` is the float 1.1. A tag does the same
job: `zip: !!str 01234`.

As in cjson, the value readers stop the program when the type is wrong or the
node is `NULL`, so call `cyaml_type()` first whenever the input decides the
shape of the data.

## Paths, changes and deletes

Paths work as in cjson: `"server.port"`, `"hosts.#0"`, and `\.` for a dot
inside a key. `cyaml_get` returns `NULL` when any part of the path is
missing, and the node it returns is borrowed.

```c
cyaml_set(doc, "server.port", 9090);        /* CYAML_INTEGER */
cyaml_set(doc, "server.name", "edge-1");    /* CYAML_STRING  */
cyaml_set(doc, "server.tls", (bool)true);   /* CYAML_BOOL; write (bool)true */
cyaml_set(doc, "server.limit", 1.0 / 0.0);  /* .inf is valid YAML */
cyaml_set(doc, "server.proxy", NULL);       /* CYAML_NULL */
cyaml_delete(doc, "server.debug");
```

The same rules as in cjson apply:

- The parent of a new key must exist.
- A list index must be within the range of the list.
- A string must be valid UTF-8.

Unlike JSON, YAML has infinities and NaN, so `cyaml_set` accepts them.

Building a tree in code also works as in cjson. `cyaml_create_*` makes
nodes, and `cyaml_list_push` and `cyaml_dictionary_set` attach them and take
ownership. Only `ccol_invalid_args` gives the child back to you; after any
other failure, the library has already freed it. The calls refuse a borrowed
node, so attach a `cyaml_clone()` of it instead:

```c
cyaml borrowed = cyaml_dictionary_get(doc, "defaults");
cyaml_dictionary_set(other, "defaults", borrowed);              /* refused */
cyaml_dictionary_set(other, "defaults", cyaml_clone(borrowed)); /* correct */
```

There is one difference: cjson detects an attempt to attach a node below one
of its own descendants, but cyaml does not. Never attach the root of a tree
inside that same tree.

A dictionary keeps insertion order, and `cyaml_dictionary_first` and
`cyaml_dictionary_next` walk through it with a cursor, just as in cjson.

## Keys are text

A dictionary maps string keys to nodes. When a document writes a key that is
not a plain string, cyaml stores a canonical text for it, so every spelling
of the same value gives the same key:

- `null:`, `~:` and an empty key all give the key `null`.
- `0x10:` and `16:` both give `16`.
- `3.10:` gives `3.1` (the shortest text of the same `double`), and `1.0:`
  gives `1`.
- A quoted key (`"3.10":`) keeps its text exactly.

To look up such a key, use its canonical text: `cyaml_get(doc, "3\\.1")`. A
list or a mapping used as a key becomes its flow-style text.

## Anchors and aliases

`&name` marks a node, and `*name` repeats it later in the same document:

```yaml
base: &b {x: 1}
copy: *b
```

Each alias becomes an independent deep copy, so after
`cyaml_set(doc, "copy.x", 2)` the tree is `{base: {x: 1}, copy: {x: 2}}`.
The serializers write the full copies instead of anchors.

## Merge keys

A `<<` key merges one mapping, or a list of mappings, into the mapping that
contains it, with these rules:

- A key that the mapping writes itself wins over a merged key.
- In a list of sources, an earlier source wins over a later one.
- The `<<` entry does not appear in the parsed tree; the merged members take
  its position in the key order.

[cyaml(7)](../man/cyaml/cyaml.7) gives the full rules.

The typical use is a set of environments that share default values:

```c
/* Per-environment settings that share their defaults through an anchor
 * and a merge key. The program prints the effective settings of one
 * environment. */
#include <stdio.h>
#include <ccollections/cyaml.h>

static void print_value(cyaml v) {
    switch (cyaml_type(v)) {
    case CYAML_NULL:    printf("null"); break;
    case CYAML_BOOL:    printf("%s", cyaml_bool_val(v) ? "true" : "false"); break;
    case CYAML_INTEGER: printf("%lld", cyaml_int_val(v)); break;
    case CYAML_FLOAT:   printf("%g", cyaml_double_val(v)); break;
    case CYAML_STRING:  printf("\"%s\"", cyaml_str_val(v)); break;
    default:            printf("<%s>", cyaml_type_str(v)); break;
    }
}

int main(int argc, char **argv) {
    const char *env = argc > 1 ? argv[1] : "production";
    const char *text =
        "defaults: &defaults\n"
        "  workers: 4\n"
        "  timeout: 30\n"
        "  debug: false\n"
        "  region: eu-west-1\n"
        "\n"
        "staging:\n"
        "  <<: *defaults\n"
        "  debug: true\n"
        "\n"
        "production:\n"
        "  <<: *defaults\n"
        "  workers: 16\n"
        "  version: '1.10'   # quoted, so it stays a string\n";

    char *err = NULL;
    cyaml doc = cyaml_parse(text, &err);
    if (!doc) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    cyaml settings = cyaml_get(doc, env);
    if (cyaml_type(settings) != CYAML_DICTIONARY) {
        fprintf(stderr, "no environment named %s\n", env);
        cyaml_destroy(doc);
        return 1;
    }

    /* After the parse, the merge key is gone from the tree and its members
     * take its place. An explicit key of the mapping wins over a merged
     * key. */
    cyaml_dictionary_iter it;
    for (bool ok = cyaml_dictionary_first(settings, &it); ok;
         ok = cyaml_dictionary_next(&it)) {
        printf("%-8s = ", it.key);
        print_value(it.value);
        printf("\n");
    }

    cyaml_destroy(doc);
    return 0;
}
```

The output for `production` is:

```
timeout  = 30
debug    = false
region   = "eu-west-1"
workers  = 16
version  = "1.10"
```

Because `workers` comes from the mapping itself, it wins over the default
value and keeps its own position after the merged members.

## Tags

A tag goes in front of a value. The core-schema tags (`!!str`, `!!int`,
`!!float`, `!!bool`, `!!null`) set the type, while every other tag is a
label that your program can read with `cyaml_node_tag()` and set with
`cyaml_node_set_tag()`.

```c
#include <stdio.h>
#include <ccollections/cyaml.h>

int main(void) {
    char *err = NULL;
    cyaml doc = cyaml_parse(
        "zip: !!str 01234\n"          /* the tag sets the type to string */
        "secret: !vault abc123\n"     /* a custom tag does not set a type */
        "on: yes\n"                   /* YAML 1.2: the two are plain strings */
        "3.10: python\n",             /* a float key, stored as 3.1 */
        &err);
    if (!doc) { fprintf(stderr, "%s\n", err); return 1; }

    cyaml zip = cyaml_get(doc, "zip");
    printf("zip: %s %s\n", cyaml_type_str(zip), cyaml_str_val(zip));
    printf("zip tag: %s\n", cyaml_node_tag(zip));

    cyaml secret = cyaml_get(doc, "secret");
    printf("secret: %s tag %s\n", cyaml_str_val(secret), cyaml_node_tag(secret));

    printf("on: %s\n", cyaml_str_val(cyaml_get(doc, "on")));
    /* In a path, write a dot in a key as \. */
    printf("3.1: %s\n", cyaml_str_val(cyaml_get(doc, "3\\.1")));

    /* Tag a node that the program built itself. */
    cyaml_set(doc, "password", "s3cret");
    cyaml_node_set_tag(cyaml_get(doc, "password"), "!vault");

    char *out = cyaml_serialize(doc);
    if (out)
        fputs(out, stdout);
    cyaml_serialize_free(out);
    cyaml_destroy(doc);
    return 0;
}
```

The output is:

```
zip: CYAML_STRING 01234
zip tag: tag:yaml.org,2002:str
secret: abc123 tag !vault
on: yes
3.1: python
zip: "01234"
secret: !<!vault> abc123
"on": "yes"
3.1: python
password: !<!vault> s3cret
```

The output shows three rules of the serializer:

- It writes a custom tag in its full `!<...>` form.
- It leaves out a core tag when quotes already keep the type.
- It quotes `on` and `yes`, so that YAML 1.1 readers such as PyYAML also
  read them as strings.

## More than one document in a stream

A stream can hold several documents separated by `---`. For such input,
`cyaml_parse` returns a `CYAML_LIST` whose elements are the roots of the
documents; for a single document, `cyaml_parse` returns that document's
root. `cyaml_serialize_stream` writes such a list back with a `---` before
each document. An anchor defined in one document is not available in the next.

## Example: edit Kubernetes manifests

```c
/* Edit a stream of Kubernetes manifests: scale each Deployment, label
 * every object, and then write the stream back. */
#include <stdio.h>
#include <string.h>
#include <ccollections/cyaml.h>

static void edit_manifest(cyaml m) {
    /* The value accessors stop the program on a wrong type, so check the
     * type of any node that the input might not contain. */
    cyaml kind = cyaml_get(m, "kind");
    if (cyaml_type(kind) == CYAML_STRING &&
        strcmp(cyaml_str_val(kind), "Deployment") == 0)
        cyaml_set(m, "spec.replicas", 5);
    /* cyaml_set needs an existing parent, so make the labels map first. */
    if (cyaml_type(cyaml_get(m, "metadata")) != CYAML_DICTIONARY) return;
    if (!cyaml_get(m, "metadata.labels")) {
        cyaml labels = cyaml_create_dictionary();
        if (labels && cyaml_dictionary_set(cyaml_get(m, "metadata"), "labels",
                                           labels) == ccol_invalid_args)
            cyaml_destroy(labels);
    }
    cyaml_set(m, "metadata.labels.team", "payments");
}

int main(void) {
    const char *stream =
        "apiVersion: v1\n"
        "kind: Service\n"
        "metadata:\n"
        "  name: api\n"
        "---\n"
        "apiVersion: apps/v1\n"
        "kind: Deployment\n"
        "metadata:\n"
        "  name: api\n"
        "  labels:\n"
        "    app: api\n"
        "spec:\n"
        "  replicas: 2\n";

    char *err = NULL;
    cyaml docs = cyaml_parse(stream, &err);
    if (!docs) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    /* Two or more documents give a list of their roots. */
    if (cyaml_type(docs) == CYAML_LIST) {
        for (size_t i = 0; i < cyaml_list_len(docs); i++)
            edit_manifest(cyaml_list_get(docs, i));
    } else {
        edit_manifest(docs);
    }

    char *out = cyaml_serialize_stream(docs);
    if (out) {
        fputs(out, stdout);
        cyaml_serialize_free(out);
    }
    cyaml_destroy(docs);
    return 0;
}
```

The output is:

```
---
apiVersion: v1
kind: Service
metadata:
  name: api
  labels:
    team: payments
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: api
  labels:
    app: api
    team: payments
spec:
  replicas: 5
```

## Write YAML text

| Function | Output |
|---|---|
| `cyaml_serialize` | block style, one key per line, for people to read |
| `cyaml_serialize_flow` | flow style on one line: `{a: 1, b: [x, y]}` |
| `cyaml_serialize_stream` | a list as a stream of `---` documents |

Each function returns a string, or `NULL` when there is not enough memory;
free the string with `cyaml_serialize_free`. Parsing the output always gives
the same tree back, and common YAML 1.1 tools read it the same way too,
because:

- the serializer quotes strings that look like other types,
- a float always contains a `.`, and
- every character that YAML does not allow raw is written as an escape.

## Strict input

`cyaml` refuses input that YAML 1.2 does not allow instead of repairing it.
A parse fails, with a message that gives the line and the column, on any of
these:

- invalid UTF-8;
- a control character that YAML does not allow raw (write it as an escape in
  a double-quoted string instead, for example `"\x01"`);
- a tab in the indentation (a tab after the indentation, as in
  `key:\tvalue`, is allowed);
- a tag whose text does not match its type, for example `!!int abc`;
- collections nested more than 500 levels deep.

An error message is always printable ASCII, so it is safe to log. A parse
also limits the number of nodes and bytes it creates, and a separate fixed
limit caps the data that anchors and merge keys can copy, so a small hostile
document (a "billion laughs") cannot use up all the memory. See
[cyaml_parse(3)](../man/cyaml/cyaml_parse.3) for all the rules and numbers.

## Example: change JSON into YAML

Because the two modules use the same shape of tree, a converter is a short
recursive walk. Note how each attachment handles the two kinds of failure.

```c
/* Convert a JSON document into YAML by walking one tree while building
 * the other. */
#include <stdio.h>
#include <ccollections/cjson.h>
#include <ccollections/cyaml.h>

static cyaml convert(cjson j) {
    switch (cjson_type(j)) {
    case CJSON_BOOL:    return cyaml_create_bool(cjson_bool_val(j));
    case CJSON_INTEGER: return cyaml_create_int(cjson_int_val(j));
    case CJSON_FLOAT:   return cyaml_create_double(cjson_double_val(j));
    case CJSON_STRING:  return cyaml_create_string(cjson_str_val(j));
    case CJSON_LIST: {
        cyaml out = cyaml_create_list();
        for (size_t i = 0; out && i < cjson_list_len(j); i++) {
            cyaml child = convert(cjson_list_get(j, i));
            ccol_retval_t r = cyaml_list_push(out, child);
            if (r != ccol_success) {
                /* Only ccol_invalid_args hands the child back to us; after
                 * any other failure, the call has freed it. */
                if (r == ccol_invalid_args) cyaml_destroy(child);
                cyaml_destroy(out);   /* this also stops the loop */
            }
        }
        return out;
    }
    case CJSON_DICTIONARY: {
        cyaml out = cyaml_create_dictionary();
        cjson_dictionary_iter it;
        for (bool ok = out && cjson_dictionary_first(j, &it); ok;
             ok = cjson_dictionary_next(&it)) {
            cyaml child = convert(it.value);
            ccol_retval_t r = cyaml_dictionary_set(out, it.key, child);
            if (r != ccol_success) {
                if (r == ccol_invalid_args) cyaml_destroy(child);
                cyaml_destroy(out);
                break;
            }
        }
        return out;
    }
    default:
        return cyaml_create_null();
    }
}

int main(void) {
    const char *json =
        "{\"name\":\"web\",\"replicas\":3,\"ratio\":0.5,\"enabled\":true,"
        "\"ports\":[80,443],\"env\":{\"MODE\":\"prod\",\"DEBUG\":null},"
        "\"version\":\"1.10\"}";

    char *err = NULL;
    cjson j = cjson_parse(json, &err);
    if (!j) { fprintf(stderr, "%s\n", err); return 1; }

    cyaml y = convert(j);
    cjson_destroy(j);
    if (!y) return 1;

    char *out = cyaml_serialize(y);
    if (out) {
        fputs(out, stdout);
        cyaml_serialize_free(out);
    }
    cyaml_destroy(y);
    return 0;
}
```

The output is:

```
name: web
replicas: 3
ratio: 0.5
enabled: true
ports:
  - 80
  - 443
env:
  MODE: prod
  DEBUG: ~
version: "1.10"
```

`version` keeps its quotes, so a parser reads it back as a string.

## Custom allocators

As in cjson, every function that makes nodes has an `_mp` variant. Each node
remembers its allocator, so `cyaml_destroy` needs no extra argument. Release
a serialized string through `cyaml_serialize_free_mp` with the procs of the
root. See [Memory management](memory.md).

## Good to know

- **Borrowed and owned nodes.** Nodes from `cyaml_get`, `cyaml_list_get`,
  `cyaml_dictionary_get` and the cursor are borrowed: do not destroy them,
  and clone them before you attach them to another container.
- **After a failed attach, free the child only on `ccol_invalid_args`.**
  When a list push or a dictionary set returns any other error, the library
  has already freed the child, so do not free it again.
- **Do not attach a node below one of its own descendants.** cyaml does not
  detect this cycle (cjson does), and a later destroy, clone or serialize of
  that tree has undefined behavior.
- **Quote values that must stay strings,** such as versions (`'1.10'`), ZIP
  codes and telephone numbers.
- **Before C23, write `(bool)true`, not `true`,** with `cyaml_set`.
- **The tree does not keep comments,** so a parse followed by a serialize
  removes them.
- **cyaml does not report a key that occurs two times.** The tree silently
  keeps the last value, so do not rely on a YAML parser to catch such keys.
- **One tree, one writer.** Many threads can read a tree as long as no
  thread changes it. See [Concurrency](concurrency.md).

## Reference

Overview: [cyaml(7)](../man/cyaml/cyaml.7) (YAML features, types, anchors,
merge keys)

Parse and write:
[cyaml_parse(3)](../man/cyaml/cyaml_parse.3),
[cyaml_parse_mp(3)](../man/cyaml/cyaml_parse_mp.3),
[cyaml_parse_n(3)](../man/cyaml/cyaml_parse_n.3),
[cyaml_parse_n_mp(3)](../man/cyaml/cyaml_parse_n_mp.3),
[cyaml_serialize(3)](../man/cyaml/cyaml_serialize.3),
[cyaml_serialize_flow(3)](../man/cyaml/cyaml_serialize_flow.3),
[cyaml_serialize_stream(3)](../man/cyaml/cyaml_serialize_stream.3),
[cyaml_serialize_free(3)](../man/cyaml/cyaml_serialize_free.3),
[cyaml_serialize_free_mp(3)](../man/cyaml/cyaml_serialize_free_mp.3)

Paths:
[cyaml_get(3)](../man/cyaml/cyaml_get.3),
[cyaml_set(3)](../man/cyaml/cyaml_set.3),
[cyaml_delete(3)](../man/cyaml/cyaml_delete.3)

Read nodes:
[cyaml_type(3)](../man/cyaml/cyaml_type.3),
[cyaml_type_str(3)](../man/cyaml/cyaml_type_str.3),
[cyaml_bool_val(3)](../man/cyaml/cyaml_bool_val.3),
[cyaml_int_val(3)](../man/cyaml/cyaml_int_val.3),
[cyaml_double_val(3)](../man/cyaml/cyaml_double_val.3),
[cyaml_str_val(3)](../man/cyaml/cyaml_str_val.3),
[cyaml_node_tag(3)](../man/cyaml/cyaml_node_tag.3),
[cyaml_node_set_tag(3)](../man/cyaml/cyaml_node_set_tag.3)

Lists:
[cyaml_list_len(3)](../man/cyaml/cyaml_list_len.3),
[cyaml_list_get(3)](../man/cyaml/cyaml_list_get.3),
[cyaml_list_push(3)](../man/cyaml/cyaml_list_push.3),
[cyaml_list_remove(3)](../man/cyaml/cyaml_list_remove.3)

Dictionaries:
[cyaml_dictionary_size(3)](../man/cyaml/cyaml_dictionary_size.3),
[cyaml_dictionary_get(3)](../man/cyaml/cyaml_dictionary_get.3),
[cyaml_dictionary_set(3)](../man/cyaml/cyaml_dictionary_set.3),
[cyaml_dictionary_remove(3)](../man/cyaml/cyaml_dictionary_remove.3),
[cyaml_dictionary_first(3)](../man/cyaml/cyaml_dictionary_first.3),
[cyaml_dictionary_next(3)](../man/cyaml/cyaml_dictionary_next.3)

Make, copy and free nodes:
[cyaml_create_null(3)](../man/cyaml/cyaml_create_null.3),
[cyaml_create_bool(3)](../man/cyaml/cyaml_create_bool.3),
[cyaml_create_int(3)](../man/cyaml/cyaml_create_int.3),
[cyaml_create_double(3)](../man/cyaml/cyaml_create_double.3),
[cyaml_create_string(3)](../man/cyaml/cyaml_create_string.3),
[cyaml_create_list(3)](../man/cyaml/cyaml_create_list.3),
[cyaml_create_dictionary(3)](../man/cyaml/cyaml_create_dictionary.3),
[cyaml_create_string_mp(3)](../man/cyaml/cyaml_create_string_mp.3) (and the
other `_mp` variants),
[cyaml_clone(3)](../man/cyaml/cyaml_clone.3),
[cyaml_destroy(3)](../man/cyaml/cyaml_destroy.3),
[cyaml_declare(3)](../man/cyaml/cyaml_declare.3),
[cyaml_declare_scoped(3)](../man/cyaml/cyaml_declare_scoped.3)

Related guides:
[cjson](cjson.md),
[Memory management](memory.md),
[Design of the type-safe macros](design.md),
[Concurrency](concurrency.md)
