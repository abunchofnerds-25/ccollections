# cstring: a growable string

`cstring` is a heap-allocated string that grows as you add text to it. It
looks after the size of the buffer itself, it adds text without `strcat` so
no call ever writes into a buffer that is too small, and it writes the
terminating NUL so you can't forget it. The content is always an ordinary
NUL-terminated C string, which you can pass to `printf`, `fopen`, `strtol`
or anything else that takes a `const char *`.

A `cstr` is worth it when you build text in small pieces (messages, paths,
reports, protocol lines) or when you edit text by trimming, replacing,
splitting or changing its case. For a short string you only read, a plain
`const char *` is the simplest thing.

```c
#include <ccollections/cstring.h>
```

## A first example

```c
#include <stdio.h>
#include <ccollections/cstring.h>

int main(void) {
    cstr_construct(greeting, "Hello");  /* a new string with this content */

    cstr_append(greeting, ", world");
    cstr_prepend(greeting, ">> ");
    cstr_append(greeting, "!");

    printf("%s (%zu characters)\n", cstr_c_str(greeting),
           cstr_length(greeting));

    cstr_destroy(greeting);             /* frees it and sets greeting to NULL */
    return 0;
}
```

Compile with `-std=gnu11` and link with `-lccollections`, and the program
prints `>> Hello, world! (16 characters)`.

## Create and destroy a string

| You want | Use |
|---|---|
| Create it now | `cstr_construct(s, "text")` |
| An empty string | `cstr_construct(s, NULL)` or `cstr_construct(s, "")` |
| Declare now, create later | `cstr_declare(s)` then `cstr_init(s, "text")` |
| Destroy it automatically at the end of the scope | `cstr_construct_scoped(s, "text")` |
| Use your own allocator | `cstr_construct_mp(s, "text", &procs)` |

`cstr_destroy(s)` frees the string and sets `s` to `NULL`. `cstr_reset(s)`
only removes the text and leaves the string ready to be used again.

The scoped form is handy in loops and in functions that can return early,
since the string is freed on every way out of the block. The lifecycle
macros that all the modules share are described in [the design
guide](design.md), and custom allocators in [Memory management](memory.md).

## Read a string

```c
cstr_c_str(s)      /* const char * to the content */
cstr_length(s)     /* number of characters, without the NUL */
cstr_is_empty(s)   /* true when the length is 0 */
cstr_at(s, i)      /* the character at i, or '\0' when i is out of range */
```

`cstr_c_str` returns a pointer into the string's buffer. Any call that
changes the string can move that buffer, so the pointer is only good until
the next change and you fetch it again after each one.

## Change a string

| Operation | Example |
|---|---|
| Add at the end | `cstr_append(s, ".txt")` |
| Add at the start | `cstr_prepend(s, "/tmp/")` |
| Add at a position | `cstr_insert(s, 3, "abc")` |
| Replace the whole content | `cstr_set(s, "new content")` |
| Replace every occurrence | `cstr_replace(s, "\t", "    ")` |
| Strip leading and trailing whitespace | `cstr_trim(s)` |
| Change case | `cstr_to_upper(s)`, `cstr_to_lower(s)` |

The arguments are plain C strings, so a literal, a `char` array or the
`cstr_c_str` of another `cstr` all work. The case functions change only the
ASCII letters `A` to `Z` and `a` to `z`, whatever the locale, and never
touch a byte above 0x7F, which keeps a UTF-8 string valid. `cstr_trim` works
the same way: it removes only ASCII white space (space, `\t`, `\n`, `\v`,
`\f` and `\r`) in every locale and leaves every byte above 0x7F alone.

If you know the final size in advance, call `cstr_reserve(s, bytes)`. It
grows the buffer once and the appends after it don't have to reallocate.

## Search and compare

```c
cstr_equals(s, "yes")          /* true or false */
cstr_compare(s, "m")           /* <0, 0 or >0, like strcmp */
cstr_starts_with(s, "http://")
cstr_ends_with(s, ".json")
cstr_find(s, "=")              /* index of the first match */
cstr_rfind(s, "/")             /* index of the last match */
```

`cstr_find` and `cstr_rfind` return `ccol_invalid_size` when nothing
matches, so compare the result with `ccol_invalid_size` every time before
you use it as an index.

## Make new strings from a string

Three operations hand you new strings that you own and have to destroy.
`cstr_substring(s, start, len)` copies a range, cut off at the end of the
string if the range runs past it. `cstr_copy(s, NULL)` copies the whole
string, and `cstr_split(s, ",", NULL)` cuts the string at each delimiter
and returns a vector of new strings.

That vector is a [cvector](cvector.md) whose elements are separate `cstr`
objects, so you destroy each element first and then the vector:

```c
#include <stdio.h>
#include <ccollections/cstring.h>

int main(void) {
    cstr_construct(line, "alice,30,,engineer");

    cvec parts = cstr_split(line, ",", NULL);
    if (!parts)
        return 1;
    cvec_redeclare(parts, cstr);       /* the elements are cstr */

    for (size_t i = 0; i < cvec_size(parts); i++) {
        printf("[%zu] '%s'\n", i, cstr_c_str(cvec_at(parts, i)));
        cstr_destroy(cvec_at(parts, i));
    }
    cvec_destroy(parts);
    cstr_destroy(line);
    return 0;
}
```

Empty fields are kept, so the program prints four tokens and the third one
is empty.

Watch out for one difference between the three: `cstr_copy` and
`cstr_split` return `NULL` when they fail, but `cstr_substring` stops the
program on failure like the other macros do.

## Use a cstr with a map

The maps ([chashmap](chashmap.md), [cbstmap](cbstmap.md)) take `char *`
keys and values and not `cstr`, so pass the content with `cstr_c_str`. The
map copies the characters into its own storage, and after the
insert you can change or destroy the `cstr` without the map noticing:

```c
char *k = (char *)cstr_c_str(key);
chmap_insert(map, k, count);
cstr_destroy(key);                     /* the map keeps its own copy */
```

The settings example below does exactly this.

## When an error must not stop the program

When an allocation fails, the `cstr_*` macros that can fail stop the
program through `_ccol_fatal_err()`. If you have to recover from that, call
the functions under the macros instead, which return a status code. Code
that ends up inside a library is better off with these too, since the
library's caller decides what a failure means:

```c
if (cstring_append(s, piece) != ccol_success) {
    /* handle the failure */
}
```

`cstring_create` and `cstring_create_full` create a string and return
`NULL` on failure, and `cstring_new(text)` is the shortest way to create one
in this layer. All of these functions are listed in the Reference section
below.

## Example: a settings file reader

Lots of programs keep their settings as `key = value` lines with `#`
comments. This one trims each line, skips empty lines and comments, cuts
each line at the first `=` and stores the pair in a hash map. A value can
contain `=` itself, which is why the key and the value come out through
`cstr_find` and `cstr_substring` and not through `cstr_split`.

```c
#include <stdio.h>
#include <ccollections/cstring.h>
#include <ccollections/chashmap.h>

static void parse_settings(const char *text, chmap settings) {
    chmap_redeclare(settings, char *, char *);

    cstr_construct_scoped(all, text);
    cvec lines = cstr_split(all, "\n", NULL);
    if (!lines)
        return;
    cvec_redeclare(lines, cstr);

    for (size_t i = 0; i < cvec_size(lines); i++) {
        cstr line = cvec_at(lines, i);
        cstr_trim(line);
        size_t eq = cstr_find(line, "=");
        if (!cstr_is_empty(line) && !cstr_starts_with(line, "#") &&
            eq != ccol_invalid_size) {
            cstr key = cstr_substring(line, 0, eq);
            cstr val = cstr_substring(line, eq + 1, cstr_length(line));
            cstr_trim(key);
            cstr_trim(val);
            char *k = (char *)cstr_c_str(key);
            char *v = (char *)cstr_c_str(val);
            chmap_insert(settings, k, v);  /* the map copies both */
            cstr_destroy(key);
            cstr_destroy(val);
        }
        cstr_destroy(cvec_at(lines, i));
    }
    cvec_destroy(lines);
}

int main(void) {
    const char *file =
        "# server settings\n"
        "host = example.org\n"
        "\n"
        "port=8080\n"
        "   greeting =  a=b   \n";

    chmap_construct(settings, char *, char *);
    parse_settings(file, settings);

    const char *names[] = {"host", "port", "greeting", "missing"};
    for (size_t i = 0; i < 4; i++) {
        char *const *v = chmap_get_ptr(settings, (char *)names[i]);
        printf("%-8s -> %s\n", names[i], v ? *v : "(not set)");
    }

    chmap_destroy(settings);
    return 0;
}
```

## Example: a mail-merge template

A message template has placeholders like `{name}`, and the program replaces
them with values from a table after escaping each value for HTML. The
escape has to replace `&` first, otherwise the `&` inside the other
entities it adds would be escaped a second time.

```c
#include <stdio.h>
#include <ccollections/cstring.h>

static void html_escape(cstr s) {
    cstr_replace(s, "&", "&amp;");      /* must run first */
    cstr_replace(s, "<", "&lt;");
    cstr_replace(s, ">", "&gt;");
    cstr_replace(s, "\"", "&quot;");
}

static void fill(cstr out, const char *placeholder, const char *value) {
    cstr_construct_scoped(safe, value);
    html_escape(safe);
    cstr_replace(out, placeholder, cstr_c_str(safe));
}

int main(void) {
    const char *customers[][2] = {
        {"Ada", "Tea & biscuits"},
        {"Bob <admin>", "\"Deluxe\" box"},
    };

    for (size_t i = 0; i < 2; i++) {
        cstr_construct_scoped(msg,
            "<p>Dear {name}, your order of {item} has shipped.</p>");
        fill(msg, "{name}", customers[i][0]);
        fill(msg, "{item}", customers[i][1]);
        printf("%s\n", cstr_c_str(msg));
    }
    return 0;
}
```

## Example: normalise file paths

The program turns paths that a user typed into one canonical form. Each
backslash becomes a slash, a run of two or more slashes becomes a single
one, a trailing slash is removed, and the file extension is shown in lower
case.

```c
#include <stdio.h>
#include <ccollections/cstring.h>

static void normalise(cstr path) {
    cstr_trim(path);
    cstr_replace(path, "\\", "/");
    while (cstr_find(path, "//") != ccol_invalid_size)
        cstr_replace(path, "//", "/");
    if (cstr_length(path) > 1 && cstr_ends_with(path, "/")) {
        cstr shorter = cstr_substring(path, 0, cstr_length(path) - 1);
        cstr_set(path, cstr_c_str(shorter));
        cstr_destroy(shorter);
    }
}

int main(void) {
    const char *inputs[] = {"  C:\\Users\\ada\\Report.PDF ",
                            "/var//log///syslog/", "notes.Txt"};

    for (size_t i = 0; i < 3; i++) {
        cstr_construct_scoped(p, inputs[i]);
        normalise(p);

        size_t slash = cstr_rfind(p, "/");
        size_t name_at = slash == ccol_invalid_size ? 0 : slash + 1;
        size_t dot = cstr_rfind(p, ".");
        printf("%-28s", cstr_c_str(p));
        if (dot != ccol_invalid_size && dot > name_at) {
            cstr ext = cstr_substring(p, dot + 1, cstr_length(p));
            cstr_to_lower(ext);
            printf(" extension: %s", cstr_c_str(ext));
            cstr_destroy(ext);
        }
        printf("\n");
    }
    return 0;
}
```

## Good to know

The pointer from `cstr_c_str` is borrowed, so never free it, and don't keep
it across an append, an insert, a replace or a set (see [Read a
string](#read-a-string)).

Every `cstr` you get from `cstr_substring`, `cstr_copy` or `cstr_split` is
yours to destroy, and so is the vector that `cstr_split` returns.

`cstr_destroy(s)` sets `s` to `NULL`, so calling it twice on the same
variable is safe. A copy of the handle in another variable isn't set to
`NULL` though, and destroying through that copy frees the memory a second
time, so keep one owner per handle.

A `cstr` holds text up to its first NUL byte. It's a C string and not a
container for binary data.

There's no lock inside a `cstr`, on purpose. If more than one thread uses a
string, protect it with your own lock (see [Concurrency](concurrency.md)).

## Reference

Overview: [cstring(7)](../man/cstring/cstring.7)

Lifecycle:
[cstr_construct(3)](../man/cstring/cstr_construct.3),
[cstr_construct_scoped(3)](../man/cstring/cstr_construct_scoped.3),
[cstr_construct_mp(3)](../man/cstring/cstr_construct_mp.3),
[cstr_construct_mp_scoped(3)](../man/cstring/cstr_construct_mp_scoped.3),
[cstr_declare(3)](../man/cstring/cstr_declare.3),
[cstr_declare_scoped(3)](../man/cstring/cstr_declare_scoped.3),
[cstr_init(3)](../man/cstring/cstr_init.3),
[cstr_init_mp(3)](../man/cstring/cstr_init_mp.3),
[cstr_reserve(3)](../man/cstring/cstr_reserve.3),
[cstr_reset(3)](../man/cstring/cstr_reset.3),
[cstr_destroy(3)](../man/cstring/cstr_destroy.3)

Reading:
[cstr_c_str(3)](../man/cstring/cstr_c_str.3),
[cstr_length(3)](../man/cstring/cstr_length.3),
[cstr_at(3)](../man/cstring/cstr_at.3),
[cstr_is_empty(3)](../man/cstring/cstr_is_empty.3)

Changing:
[cstr_append(3)](../man/cstring/cstr_append.3),
[cstr_prepend(3)](../man/cstring/cstr_prepend.3),
[cstr_insert(3)](../man/cstring/cstr_insert.3),
[cstr_set(3)](../man/cstring/cstr_set.3),
[cstr_replace(3)](../man/cstring/cstr_replace.3),
[cstr_trim(3)](../man/cstring/cstr_trim.3),
[cstr_to_upper(3)](../man/cstring/cstr_to_upper.3),
[cstr_to_lower(3)](../man/cstring/cstr_to_lower.3)

Searching and comparing:
[cstr_equals(3)](../man/cstring/cstr_equals.3),
[cstr_compare(3)](../man/cstring/cstr_compare.3),
[cstr_starts_with(3)](../man/cstring/cstr_starts_with.3),
[cstr_ends_with(3)](../man/cstring/cstr_ends_with.3),
[cstr_find(3)](../man/cstring/cstr_find.3),
[cstr_rfind(3)](../man/cstring/cstr_rfind.3)

New strings:
[cstr_substring(3)](../man/cstring/cstr_substring.3),
[cstr_copy(3)](../man/cstring/cstr_copy.3),
[cstr_split(3)](../man/cstring/cstr_split.3)

Raw functions (they give a status and do not stop the program):
[cstring_new(3)](../man/cstring/cstring_new.3),
[cstring_create(3)](../man/cstring/cstring_create.3),
[cstring_create_full(3)](../man/cstring/cstring_create_full.3),
[cstring_destroy(3)](../man/cstring/cstring_destroy.3),
[cstring_append(3)](../man/cstring/cstring_append.3),
[cstring_prepend(3)](../man/cstring/cstring_prepend.3),
[cstring_insert(3)](../man/cstring/cstring_insert.3),
[cstring_set(3)](../man/cstring/cstring_set.3),
[cstring_replace(3)](../man/cstring/cstring_replace.3),
[cstring_reserve(3)](../man/cstring/cstring_reserve.3),
[cstring_reset(3)](../man/cstring/cstring_reset.3),
[cstring_substring(3)](../man/cstring/cstring_substring.3),
[cstring_copy(3)](../man/cstring/cstring_copy.3),
[cstring_split(3)](../man/cstring/cstring_split.3),
[cstring_get_mprocs(3)](../man/cstring/cstring_get_mprocs.3)

Related guides:
[Vectors](cvector.md),
[Hash maps](chashmap.md),
[Design of the type-safe macros](design.md),
[Memory management](memory.md),
[Concurrency](concurrency.md)
