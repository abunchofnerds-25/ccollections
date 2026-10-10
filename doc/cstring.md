# cstring: a growable string

`cstring` is a heap-allocated string that grows as you add text to it. The
module takes care of the details for you:

- It manages the size of the buffer.
- It adds text without `strcat`, so no call ever writes into a buffer that
  is too small.
- It adds the terminating NUL, so you cannot forget it.

The content is always an ordinary NUL-terminated C string, so you can pass
it to `printf`, `fopen`, `strtol` or any other function that takes a
`const char *`.

Use a `cstr` when you build text in small pieces, such as messages, paths,
reports or protocol lines, and when you edit text: trimming, replacing,
splitting or changing its case. For a short string that you only read, a
plain `const char *` is the simplest choice.

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

Compile the program with `-std=gnu11` and link it with `-lccollections`. It
prints `>> Hello, world! (16 characters)`.

## Create and destroy a string

| You want | Use |
|---|---|
| Create it now | `cstr_construct(s, "text")` |
| An empty string | `cstr_construct(s, NULL)` or `cstr_construct(s, "")` |
| Declare now, create later | `cstr_declare(s)` then `cstr_init(s, "text")` |
| Destroy it automatically at the end of the scope | `cstr_construct_scoped(s, "text")` |
| Use your own allocator | `cstr_construct_mp(s, "text", &procs)` |

`cstr_destroy(s)` frees the string and sets `s` to `NULL`, while
`cstr_reset(s)` removes all the text and keeps the string ready for reuse.

The scoped form is useful in loops and in functions that can return early,
because the string is freed on every path out of the block. See [the design
guide](design.md) for the lifecycle macros that all the modules share, and
[Memory management](memory.md) for custom allocators.

## Read a string

```c
cstr_c_str(s)      /* const char * to the content */
cstr_length(s)     /* number of characters, without the NUL */
cstr_is_empty(s)   /* true when the length is 0 */
cstr_at(s, i)      /* the character at i, or '\0' when i is out of range */
```

`cstr_c_str` returns a pointer into the string's buffer. That pointer is
valid only until the next call that changes the string, because that call
can move the buffer, so fetch it again after every change.

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

The arguments are plain C strings, so you can pass a literal, a `char` array
or the `cstr_c_str` of another `cstr`.

When you know the final size in advance, call `cstr_reserve(s, bytes)`: it
grows the buffer once, and the appends that follow do not need to
reallocate.

## Search and compare

```c
cstr_equals(s, "yes")          /* true or false */
cstr_compare(s, "m")           /* <0, 0 or >0, like strcmp */
cstr_starts_with(s, "http://")
cstr_ends_with(s, ".json")
cstr_find(s, "=")              /* index of the first match */
cstr_rfind(s, "/")             /* index of the last match */
```

`cstr_find` and `cstr_rfind` return `ccol_invalid_size` when there is no
match, so always compare the result with `ccol_invalid_size` before you use
it as an index.

## Make new strings from a string

Three operations give you new strings, which you own and must destroy:

- `cstr_substring(s, start, len)` copies a range. If the range runs past the
  end of the string, the macro cuts it off at the end.
- `cstr_copy(s, NULL)` copies the whole string.
- `cstr_split(s, ",", NULL)` cuts the string at each delimiter and returns a
  vector of new strings.

`cstr_split` returns a [cvector](cvector.md) in which each element is a
separate `cstr`, so destroy each element first and then the vector:

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

The macro keeps empty fields, so this program prints four tokens, the third
of which is empty.

`cstr_copy` and `cstr_split` return `NULL` when they fail, whereas
`cstr_substring`, like the other macros, stops the program on failure.

## Use a cstr with a map

The maps ([chashmap](chashmap.md), [cbstmap](cbstmap.md)) accept `char *`
keys and values, not `cstr`, so pass the content with `cstr_c_str`. The map
copies the characters into its own storage at once, which means you can
change or destroy the `cstr` after the insert without affecting the map:

```c
char *k = (char *)cstr_c_str(key);
chmap_insert(map, k, count);
cstr_destroy(key);                     /* the map keeps its own copy */
```

The settings example below does the same thing.

## When an error must not stop the program

When an allocation fails, the `cstr_*` macros that can fail stop the program
through `ccol_fatal_err()`. If you need to recover from the failure instead,
call the functions underneath the macros, which return a status code:

```c
if (cstring_append(s, piece) != ccol_success) {
    /* handle the failure */
}
```

`cstring_create` and `cstring_create_full` create a string and return `NULL`
when they fail; `cstring_new(text)` is the shortest way to create a string
in this layer. The Reference section below lists all these functions.

## Example: a settings file reader

Many programs keep their settings as `key = value` lines with `#` comments.
This program:

1. Trims each line.
2. Skips empty lines and comments.
3. Cuts each line at the first `=`.
4. Stores the pair in a hash map.

Because a value can itself contain `=`, the program extracts the key and the
value with `cstr_find` and `cstr_substring` instead of `cstr_split`.

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

A message template contains placeholders such as `{name}`, and the program
replaces them with values from a table, escaping each value for HTML. The
escape replaces `&` first, so that the `&` inside the other entities it adds
is not escaped a second time.

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

The program turns paths that a user typed into a canonical form:

- Each backslash becomes a slash.
- Two or more adjacent slashes become a single slash.
- A trailing slash is removed.
- The file extension is shown in lower case.

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

- **`cstr_c_str` returns a borrowed pointer.** It points into the string's
  buffer and becomes invalid after any change to the string. Never free it,
  and do not keep it across an append, an insert, a replace or a set.
- **You own the strings that you make from a string.** Destroy every `cstr`
  that you get from `cstr_substring`, `cstr_copy` or `cstr_split`, and also
  destroy the vector from `cstr_split`.
- **Each handle has one owner.** `cstr_destroy(s)` sets `s` to `NULL`, so
  calling it twice on the same variable is safe. A copy of the handle in
  another variable is not set to `NULL`, however, and a destroy through that
  copy frees the memory a second time.
- **The content is a C string.** A `cstr` holds the text up to its first NUL
  byte; it is not a container for binary data.
- **Thread safety.** A `cstr` deliberately has no internal lock. When more
  than one thread uses a string, protect it with your own lock. See
  [Concurrency](concurrency.md).

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
