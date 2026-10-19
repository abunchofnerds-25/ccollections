# cjson: read, change and write JSON

`cjson` turns JSON text into a tree of nodes in memory. You can read and
change any part of that tree, and when you're done `cjson` writes it back as
JSON. The parser is strict and refuses everything RFC 8259 doesn't allow,
the tree (the DOM) can be changed in place, and there are two serializers,
one for compact text and one for indented text.

`cjson` fits wherever a whole document sits comfortably in memory: a
configuration file, the body of an HTTP request or response, a message on a
queue, a test fixture. Two short path macros, `cjson_get` and `cjson_set`,
reach deep into a document in one call, so most programs never walk the
tree by hand. It's the wrong tool when you have to process a JSON stream
much larger than memory while it arrives, or when you need integers larger
than 64 bits with no loss of precision.

```c
#include <ccollections/cjson.h>
```

For YAML data, [cyaml](cyaml.md) has the same API with a `cyaml_` prefix.

## A first example

```c
#include <stdbool.h>
#include <stdio.h>
#include <ccollections/cjson.h>

int main(void) {
    const char *text =
        "{\"name\": \"Alice\", \"age\": 30, \"tags\": [\"admin\", \"ops\"]}";

    char *err = NULL;
    cjson doc = cjson_parse(text, &err);
    if (!doc) {
        fprintf(stderr, "parse error: %s\n", err); /* do not free err */
        return 1;
    }

    /* Read: a path goes through objects by key and through arrays by #index. */
    printf("name: %s\n", cjson_str_val(cjson_get(doc, "name")));
    printf("age:  %lld\n", cjson_int_val(cjson_get(doc, "age")));
    printf("tag0: %s\n", cjson_str_val(cjson_get(doc, "tags.#0")));

    /* Write: cjson_set creates a missing key or replaces an existing one. */
    cjson_set(doc, "age", 31);
    cjson_set(doc, "active", (bool)true);

    char *out = cjson_serialize(doc);   /* NULL when there is not enough memory */
    if (out)
        printf("%s\n", out);
    cjson_serialize_free(out);

    cjson_destroy(doc);
    return 0;
}
```

The path macros use GNU C extensions, so build with `-std=gnu11`, and link
with `-lccollections`:

```sh
gcc -std=gnu11 first.c -lccollections -o first
```

The program prints:

```
name: Alice
age:  30
tag0: admin
{"name":"Alice","age":31,"tags":["admin","ops"],"active":true}
```

A few things from this example hold for the whole module. `cjson_parse`
returns `NULL` for bad input and points `err` at a message, which belongs to
the library, so print it or copy it but don't free it. You destroy the root
with `cjson_destroy` and that frees every node below it. A string from a
serializer belongs to you, and you release it with `cjson_serialize_free`.

## The node types

Every JSON value is a `cjson` handle, and `cjson_type()` tells you its type:

| Type | JSON | Read it with |
|---|---|---|
| `CJSON_NULL` | `null` | (no value to read) |
| `CJSON_BOOL` | `true`, `false` | `cjson_bool_val` |
| `CJSON_INTEGER` | a number with no `.` and no exponent that fits in a `long long` | `cjson_int_val` |
| `CJSON_FLOAT` | any other number, as a `double` | `cjson_double_val` |
| `CJSON_STRING` | a string, always valid UTF-8 | `cjson_str_val` |
| `CJSON_LIST` | an array | `cjson_list_len`, `cjson_list_get` |
| `CJSON_DICTIONARY` | an object | `cjson_dictionary_size`, `cjson_dictionary_get` |

The value readers are strict, and `cjson_int_val` on a string or on `NULL`
stops the program. That's what you want when the type is part of your
program's logic, but not when the input decides the type, so for data from
outside your program check the type first:

```c
cjson port = cjson_get(doc, "server.port");
if (cjson_type(port) != CJSON_INTEGER) {   /* NULL reads as CJSON_NULL */
    fprintf(stderr, "server.port: expected an integer, got %s\n",
            cjson_type_str(port));
    return -1;
}
long long p = cjson_int_val(port);
```

`cjson_type_str()` gives you the name of the type for a message like this
one.

A number is a `CJSON_INTEGER` only when it has no `.`, no exponent and fits
in 64 bits, so `5.0` and `1e3` are floats. If your program takes "any
number", accept both types, as the configuration loader below does.

## Paths

A path is a list of components separated by dots. On an object a component
is a key, as in `"server.port"`, and on an array `#N` is the element at
index `N`, as in `"users.#0.name"`. A dot inside a key is written `\.` and a
backslash `\\`, and since C source doubles the backslash again,
`cjson_get(doc, "a\\.b")` reads the key `a.b`.

`cjson_get` returns `NULL` when any part of the path is missing, which lets
you look up optional fields without checking each level. An empty path
gives you the root.

The node `cjson_get` returns is borrowed. The tree keeps owning it, it stays
valid until you change or destroy the tree, and you must not free it. The
text from `cjson_str_val` is borrowed the same way and stays valid until the
node gets a new value or is destroyed, so copy it if you need it longer.

## Change a document

`cjson_set(root, path, value)` writes a C value to a path, and the C type
of the value decides the JSON type:

```c
cjson_set(doc, "server.port", 9090);           /* CJSON_INTEGER */
cjson_set(doc, "server.ratio", 0.75);          /* CJSON_FLOAT   */
cjson_set(doc, "server.name", "edge-1");       /* CJSON_STRING  */
cjson_set(doc, "server.tls", (bool)true);      /* CJSON_BOOL    */
cjson_set(doc, "server.proxy", NULL);          /* CJSON_NULL    */
```

Write `(bool)true` and not `true`. Before C23 the macro `true` is the
integer 1, so `cjson_set(doc, "k", true)` stores the number 1.

`cjson_set` creates a missing key at the end of the path, but the parent has
to exist already. A missing parent object needs `cjson_create_dictionary`
and `cjson_dictionary_set` first. An array index has to exist too, because
`cjson_set` never makes an array longer; you add an element at the end with
`cjson_list_push`. An existing value of any type, a whole object or array
included, is replaced and the old value is freed.

`cjson_set` returns a `ccol_retval_t`. A string that isn't valid UTF-8, a
`double` that isn't finite and a C type it doesn't support (a struct, a
`long double`, most pointers) are refused with `ccol_invalid_args`, and the
document is left unchanged.

Removing a value is `cjson_delete`:

```c
cjson_delete(doc, "config.debug");   /* an object member */
cjson_delete(doc, "items.#2");       /* an array element; the elements after it move down */
```

If you already hold the parent, `cjson_dictionary_remove(obj, key)` and
`cjson_list_remove(arr, index)` do the same job without a path.

`cjson_set` and `cjson_delete` can tell a bad path from an absent one. An
empty component (two dots together, or a dot at either end), a component
that isn't valid UTF-8, a malformed `#N` on an array (`#x`, `#-1` or a bare
`#`), and a path whose shape can't apply to the tree give
`ccol_invalid_args` wherever they appear in the path. That last case is a
component that would have to go into or below a string, a number, a bool or
null (`s.k` when `s` is a string), or a name that isn't `#N` on an array.
Only a well-formed name or index that is absent gives `ccol_key_not_found`.
`cjson_get` returns NULL in both cases, and `cyaml` follows the same rule.

## Build a tree in code

Each `cjson_create_*` function makes one node, and two calls attach nodes
to a container:

```c
cjson user = cjson_create_dictionary();
cjson_dictionary_set(user, "name", cjson_create_string("Alice"));
cjson_dictionary_set(user, "age", cjson_create_int(30));

cjson roles = cjson_create_list();
cjson_list_push(roles, cjson_create_string("admin"));
cjson_dictionary_set(user, "roles", roles);
```

`cjson_list_push` and `cjson_dictionary_set` take ownership of the child,
and the return code alone tells you what to do when something goes wrong.
On `ccol_success` the container owns the child. On `ccol_invalid_args` the
call did nothing and the child stays yours (or belongs to whichever
container held it before); that code covers a `NULL` child, a child that
sits in another container, the wrong kind of container, a bad key and an
attach that would make a cycle. With any other code (not enough memory, a
full container) the library has already freed the child, so don't free it
again.

The word counter below wraps that rule in two small helper functions, and
in a program that builds many nodes it's a good habit to copy.

A borrowed node, one that comes from `cjson_get`, `cjson_list_get`,
`cjson_dictionary_get` or a dictionary cursor, already has a parent, so
attaching it is refused. To copy data from one place to another, attach a
`cjson_clone()` of it.

## Walk through an object

An object keeps its members in insertion order, which is the order of the
text for a parsed document and the order of your calls for one you build.
Replacing a value leaves the member where it was. The serializers use the
same order, so a parse followed by a serialize keeps the member order of the
source and it's the same on every run.

The cursor visits every member:

```c
cjson_dictionary_iter it;
for (bool ok = cjson_dictionary_first(obj, &it); ok;
     ok = cjson_dictionary_next(&it)) {
    printf("%s is a %s\n", it.key, cjson_type_str(it.value));
}
```

It lives on your stack, allocates no memory and needs no cleanup. During
the walk you may remove the current member
(`cjson_dictionary_remove(obj, it.key)`) and replace values, but don't
remove the member *after* the current one, because that makes the cursor
invalid. An array
doesn't need a cursor; loop from 0 to `cjson_list_len()` and call
`cjson_list_get()`.

If a parsed object has the same key twice, the tree keeps the last value at
the key's first position, so `{"a":1,"b":2,"a":3}` becomes `a = 3`, `b = 2`.

## Write JSON text

```c
char *compact = cjson_serialize(doc);           /* {"a":1,"b":[1,2]} */
char *pretty  = cjson_serialize_pretty(doc, 2); /* indent of two spaces */
cjson_serialize_free(compact);
cjson_serialize_free(pretty);
```

Both functions return `NULL` when there isn't enough memory. What they write
is always valid JSON, since every string in a tree is valid UTF-8, numbers
use `.` whatever the locale, and a float is written with the fewest digits
that read back as the same `double`.

## Strict input

`cjson` refuses incorrect input instead of guessing what it means, on
purpose. A parse
fails, with a message that names the problem and its byte offset, on any of
the following:

- a syntax error, for example a comma at the end of a list or a string in
  single quotes;
- a string or key that is not well-formed UTF-8, or a lone UTF-16 surrogate
  escape such as `\uD800`;
- the escape `\u0000`, because a C string cannot hold a null byte;
- arrays and objects nested more than 500 levels deep;
- a number too large for a `double`.

The message is always printable ASCII, so you can log it safely even when
the input comes from an attacker. The functions that take strings from your
program (`cjson_create_string`, `cjson_set` and `cjson_dictionary_set`)
refuse invalid UTF-8 as well.

A parsed tree takes memory in proportion to the length of its input, at most
about 65 bytes per input byte on a 64-bit machine. An object stays fast even
when a peer picks keys that are meant to collide. If you want to limit the
memory a hostile peer can make you use, limit the length of the input you
accept.

## Example: a configuration loader

The loader sets the defaults first and then lets the document replace the
values it names. A field with the wrong type gives an error that names the
field, instead of a crash or a silent default.

```c
/* A small configuration loader: it reads typed values with defaults and
 * gives a clear message when a field has the wrong type. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ccollections/cjson.h>

typedef struct {
    char host[64];
    long long port;
    double timeout_s;
    bool verbose;
    size_t backend_count;
} server_config;

/* Returns false and prints a message when the field exists but has the
 * wrong type. If the field is missing, *out keeps the default. */
static bool read_int(cjson doc, const char *path, long long *out) {
    cjson n = cjson_get(doc, path);
    if (!n) return true;
    if (cjson_type(n) != CJSON_INTEGER) {
        fprintf(stderr, "%s: expected CJSON_INTEGER, got %s\n", path,
                cjson_type_str(n));
        return false;
    }
    *out = cjson_int_val(n);
    return true;
}

static bool read_number(cjson doc, const char *path, double *out) {
    cjson n = cjson_get(doc, path);
    if (!n) return true;
    if (cjson_type(n) == CJSON_INTEGER) {      /* accept 5 and 5.0 */
        *out = (double)cjson_int_val(n);
        return true;
    }
    if (cjson_type(n) != CJSON_FLOAT) {
        fprintf(stderr, "%s: expected a number, got %s\n", path,
                cjson_type_str(n));
        return false;
    }
    *out = cjson_double_val(n);
    return true;
}

static bool read_bool(cjson doc, const char *path, bool *out) {
    cjson n = cjson_get(doc, path);
    if (!n) return true;
    if (cjson_type(n) != CJSON_BOOL) {
        fprintf(stderr, "%s: expected CJSON_BOOL, got %s\n", path,
                cjson_type_str(n));
        return false;
    }
    *out = cjson_bool_val(n);
    return true;
}

static bool read_string(cjson doc, const char *path, char *buf, size_t cap) {
    cjson n = cjson_get(doc, path);
    if (!n) return true;
    if (cjson_type(n) != CJSON_STRING) {
        fprintf(stderr, "%s: expected CJSON_STRING, got %s\n", path,
                cjson_type_str(n));
        return false;
    }
    snprintf(buf, cap, "%s", cjson_str_val(n));
    return true;
}

static bool load_config(const char *text, server_config *cfg) {
    /* Set the defaults first; the document overrides each value it names. */
    snprintf(cfg->host, sizeof(cfg->host), "0.0.0.0");
    cfg->port = 8080;
    cfg->timeout_s = 2.5;
    cfg->verbose = false;
    cfg->backend_count = 0;

    char *err = NULL;
    cjson doc = cjson_parse(text, &err);
    if (!doc) {
        fprintf(stderr, "config: %s\n", err);
        return false;
    }

    bool ok = read_string(doc, "server.host", cfg->host, sizeof(cfg->host)) &&
              read_int(doc, "server.port", &cfg->port) &&
              read_number(doc, "server.timeout", &cfg->timeout_s) &&
              read_bool(doc, "logging.verbose", &cfg->verbose);

    cjson backends = cjson_get(doc, "backends");
    if (ok && backends) {
        if (cjson_type(backends) != CJSON_LIST) {
            fprintf(stderr, "backends: expected a list\n");
            ok = false;
        } else {
            cfg->backend_count = cjson_list_len(backends);
            for (size_t i = 0; ok && i < cfg->backend_count; i++) {
                char path[32], url[128] = "";
                snprintf(path, sizeof(path), "#%zu.url", i);
                ok = read_string(backends, path, url, sizeof(url));
                if (ok) printf("backend %zu: %s\n", i, url);
            }
        }
    }

    cjson_destroy(doc);
    return ok;
}

int main(void) {
    const char *good =
        "{\n"
        "  \"server\":  {\"host\": \"127.0.0.1\", \"port\": 9000},\n"
        "  \"logging\": {\"verbose\": true},\n"
        "  \"backends\": [{\"url\": \"http://10.0.0.1\"},\n"
        "               {\"url\": \"http://10.0.0.2\"}]\n"
        "}\n";
    server_config cfg;
    if (load_config(good, &cfg))
        printf("%s:%lld timeout=%.1fs verbose=%d backends=%zu\n", cfg.host,
               cfg.port, cfg.timeout_s, cfg.verbose, cfg.backend_count);

    /* The loader reports a wrong type instead of accepting it silently. */
    if (!load_config("{\"server\": {\"port\": \"9000\"}}", &cfg))
        printf("rejected a bad config\n");

    /* The loader also reports text that is not correct JSON. */
    if (!load_config("{\"server\": {\"port\": 9000,}}", &cfg))
        printf("rejected malformed JSON\n");
    return 0;
}
```

## Example: a word counter that reports in JSON

Here the whole document is built in code. The `put` and `push` helpers hold
the ownership rule from above, so the rest of the program doesn't have to
think about it.

```c
/* Count the words of a text and report the result as a JSON document
 * that the code builds as a tree. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <ccollections/cjson.h>

/* Attach a child that the caller has just made. cjson_dictionary_set takes
 * the child on success and on every failure except ccol_invalid_args, so
 * only that code leaves us a child to free. A failed create (a NULL child)
 * leaves nothing to free. */
static ccol_retval_t put(cjson obj, const char *key, cjson child) {
    if (!child) return ccol_not_enough_memory;
    ccol_retval_t r = cjson_dictionary_set(obj, key, child);
    if (r == ccol_invalid_args) cjson_destroy(child);
    return r;
}

static ccol_retval_t push(cjson list, cjson child) {
    if (!child) return ccol_not_enough_memory;
    ccol_retval_t r = cjson_list_push(list, child);
    if (r == ccol_invalid_args) cjson_destroy(child);
    return r;
}

int main(void) {
    const char *text =
        "The quick brown fox jumps over the lazy dog. The dog sleeps; "
        "the fox does not.";

    cjson report = cjson_create_dictionary();
    cjson words = cjson_create_dictionary();
    if (!report || !words) {
        cjson_destroy(report);   /* a NULL handle does nothing */
        cjson_destroy(words);
        return 1;
    }
    long long total = 0;

    char word[64];
    size_t len = 0;
    for (const char *p = text;; p++) {
        if (isalpha((unsigned char)*p) && len + 1 < sizeof(word)) {
            word[len++] = (char)tolower((unsigned char)*p);
            continue;
        }
        if (len > 0) {
            word[len] = '\0';
            len = 0;
            total++;
            cjson seen = cjson_dictionary_get(words, word);
            long long count = seen ? cjson_int_val(seen) : 0;
            /* Replacing a value keeps the key in its original position. */
            if (put(words, word, cjson_create_int(count + 1)) != ccol_success)
                break;
        }
        if (*p == '\0') break;
    }

    /* The words that occur more than once, in the order of their first
     * occurrence. */
    cjson repeated = cjson_create_list();
    cjson_dictionary_iter it;
    for (bool ok = cjson_dictionary_first(words, &it); ok;
         ok = cjson_dictionary_next(&it)) {
        if (cjson_int_val(it.value) > 1)
            push(repeated, cjson_create_string(it.key));
    }

    put(report, "total", cjson_create_int(total));
    put(report, "distinct", cjson_create_int((long long)cjson_dictionary_size(words)));
    put(report, "repeated", repeated);  /* from here on, the report owns both nodes */
    put(report, "counts", words);

    char *out = cjson_serialize_pretty(report, 2);
    if (out) {
        printf("%s\n", out);
        cjson_serialize_free(out);
    }
    cjson_destroy(report);  /* also frees all the nodes below it */
    return 0;
}
```

Part of the output:

```
{
  "total": 16,
  "distinct": 11,
  "repeated": [
    "the",
    "fox",
    "dog"
  ],
  "counts": {
    "the": 4,
    ...
```

## Example: clean an event before you log it

This example walks a parsed document whose shape isn't known in advance. It
removes the null members, masks secrets at every level, copies a sub-object
with `cjson_clone` and deletes an array element by path.

```c
/* Clean up an event before logging it: remove the null members, mask the
 * secrets at every level, and copy one sub-object to the top level. */
#include <stdio.h>
#include <string.h>
#include <ccollections/cjson.h>

static bool is_secret(const char *key) {
    return strcmp(key, "password") == 0 || strcmp(key, "token") == 0;
}

static void scrub(cjson node) {
    if (cjson_type(node) == CJSON_LIST) {
        for (size_t i = 0; i < cjson_list_len(node); i++)
            scrub(cjson_list_get(node, i));
        return;
    }
    if (cjson_type(node) != CJSON_DICTIONARY) return;

    cjson_dictionary_iter it;
    for (bool ok = cjson_dictionary_first(node, &it); ok;
         ok = cjson_dictionary_next(&it)) {
        if (cjson_type(it.value) == CJSON_NULL) {
            /* The cursor stays valid when you remove the current member. */
            cjson_dictionary_remove(node, it.key);
        } else if (is_secret(it.key)) {
            /* A replacement keeps the member in its position. it.value is
             * the old node, which this call frees, so do not use it below.
             * A secret must not reach the log: if the call cannot store
             * the mask, remove the member. The call refuses a NULL child
             * with ccol_invalid_args, which leaves nothing to free; after
             * any other failure, the call has freed the child. */
            if (cjson_dictionary_set(node, it.key,
                                     cjson_create_string("***")) != ccol_success)
                cjson_dictionary_remove(node, it.key);
        } else {
            scrub(it.value);
        }
    }
}

int main(void) {
    const char *event =
        "{\"type\":\"login\",\"error\":null,"
        "\"user\":{\"name\":\"bob\",\"password\":\"hunter2\",\"team\":null},"
        "\"sessions\":[{\"id\":1,\"token\":\"abc\"},{\"id\":2,\"token\":\"def\"}]}";

    char *err = NULL;
    cjson doc = cjson_parse(event, &err);
    if (!doc) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    scrub(doc);

    /* cjson_get returns a borrowed node that the tree keeps owning. To put
     * the same data somewhere else, attach a clone of it. */
    cjson user = cjson_get(doc, "user");
    cjson copy = cjson_clone(user);
    if (copy && cjson_dictionary_set(doc, "actor", copy) == ccol_invalid_args)
        cjson_destroy(copy);

    /* A path can also delete a value. */
    cjson_delete(doc, "sessions.#1");

    char *out = cjson_serialize(doc);
    if (out)
        printf("%s\n", out);
    cjson_serialize_free(out);
    cjson_destroy(doc);
    return 0;
}
```

The output is:

```
{"type":"login","user":{"name":"bob","password":"***"},"sessions":[{"id":1,"token":"***"}],"actor":{"name":"bob","password":"***"}}
```

## Custom allocators

Every function that makes nodes has an `_mp` variant that takes a
`ccol_memmgmt_procs_t *`. Each node remembers its allocator, so
`cjson_destroy` needs no extra argument. The one thing to remember is that a
serialized string comes from the allocator of the root node, and you release
it through `cjson_serialize_free_mp` with the same procs.

```c
#include <stdio.h>
#include <stdlib.h>
#include <ccollections/cjson.h>

static size_t live_blocks;

static void *count_malloc(size_t n) {
    void *q = malloc(n);
    if (q) live_blocks++;
    return q;
}
static void *count_calloc(size_t c, size_t n) {
    void *q = calloc(c, n);
    if (q) live_blocks++;
    return q;
}
static void *count_realloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (q && !p) live_blocks++;   /* realloc(NULL, n) makes a new block */
    return q;
}
static void count_free(void *p) { if (p) live_blocks--; free(p); }

int main(void) {
    ccol_memmgmt_procs_t procs = {
        .malloc = count_malloc, .free = count_free,
        .calloc = count_calloc, .realloc = count_realloc};

    char *err = NULL;
    cjson doc = cjson_parse_mp("{\"a\":[1,2,3],\"b\":\"x\"}", &err, &procs);
    if (!doc) { fprintf(stderr, "%s\n", err); return 1; }

    /* The new nodes for this tree come from the same allocator. */
    cjson_dictionary_set(doc, "c", cjson_create_int_mp(7, &procs));

    /* The output buffer comes from the allocator of the root node, so give
     * it back through the _mp free function with the same procs. */
    char *out = cjson_serialize(doc);
    if (out)
        printf("%s (live blocks: %zu)\n", out, live_blocks);
    cjson_serialize_free_mp(out, &procs);

    cjson_destroy(doc);   /* each node frees itself through procs */
    printf("live blocks after destroy: %zu\n", live_blocks);
    return 0;
}
```

The library keeps its own copy of the procs struct, so yours can live on the
stack. The full contract is in [Memory management](memory.md).

## Good to know

`cjson_get`, `cjson_list_get`, `cjson_dictionary_get` and the cursor give
you borrowed nodes. Don't destroy them or attach them to another container;
attach a clone instead. After a failed attach, free the child only on
`ccol_invalid_args`, because for any other error from `cjson_list_push` or
`cjson_dictionary_set` the library has already freed it.

`cjson_int_val` and the other value readers stop the program when the type
is wrong or the node is `NULL`, so check the types of data that comes from
outside your program. And before C23, pass `(bool)true` to `cjson_set`, not
`true`.

During a walk, `cjson_dictionary_set` on the current key frees `it.value`,
so read the new value with `cjson_dictionary_get(obj, it.key)`. `cjson_set`
changes the value in place instead, and `it.value` stays valid.

An integer larger than a `long long` becomes a `double`, and a `double` is
exact only up to 2^53.

A tree isn't thread safe. Many threads can read the same tree as long as no
thread changes it, and separate trees are independent of each other (see
[Concurrency](concurrency.md)).

`cjson_declare_scoped(doc)` declares a handle that is destroyed at the end
of its block (GCC and Clang).

## Reference

Overview: [cjson(7)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson.7.html)

Parse and write:
[cjson_parse(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_parse.3.html),
[cjson_parse_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_parse_mp.3.html),
[cjson_parse_n(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_parse_n.3.html),
[cjson_parse_n_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_parse_n_mp.3.html),
[cjson_serialize(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_serialize.3.html),
[cjson_serialize_pretty(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_serialize_pretty.3.html),
[cjson_serialize_free(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_serialize_free.3.html),
[cjson_serialize_free_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_serialize_free_mp.3.html)

Paths:
[cjson_get(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_get.3.html),
[cjson_set(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_set.3.html),
[cjson_delete(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_delete.3.html)

Read nodes:
[cjson_type(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_type.3.html),
[cjson_type_str(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_type_str.3.html),
[cjson_bool_val(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_bool_val.3.html),
[cjson_int_val(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_int_val.3.html),
[cjson_double_val(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_double_val.3.html),
[cjson_str_val(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_str_val.3.html)

Arrays:
[cjson_list_len(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_list_len.3.html),
[cjson_list_get(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_list_get.3.html),
[cjson_list_push(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_list_push.3.html),
[cjson_list_remove(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_list_remove.3.html)

Objects:
[cjson_dictionary_size(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_dictionary_size.3.html),
[cjson_dictionary_get(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_dictionary_get.3.html),
[cjson_dictionary_set(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_dictionary_set.3.html),
[cjson_dictionary_remove(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_dictionary_remove.3.html),
[cjson_dictionary_first(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_dictionary_first.3.html),
[cjson_dictionary_next(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_dictionary_next.3.html)

Make, copy and free nodes:
[cjson_create_null(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_create_null.3.html),
[cjson_create_bool(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_create_bool.3.html),
[cjson_create_int(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_create_int.3.html),
[cjson_create_double(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_create_double.3.html),
[cjson_create_string(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_create_string.3.html),
[cjson_create_list(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_create_list.3.html),
[cjson_create_dictionary(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_create_dictionary.3.html),
[cjson_create_null_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_create_null_mp.3.html) (and the
other `_mp` variants),
[cjson_clone(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_clone.3.html),
[cjson_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_destroy.3.html),
[cjson_declare(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_declare.3.html),
[cjson_declare_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_declare_scoped.3.html)

Related guides:
[cyaml](cyaml.md),
[Memory management](memory.md),
[Design of the type-safe macros](design.md),
[Concurrency](concurrency.md)
