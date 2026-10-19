# citerators: one loop for every container

`citerators` is the iteration API that [cvector](cvector.md),
[chashmap](chashmap.md) and [cbstmap](cbstmap.md) share. You write the same
loop for any of them and get typed pointers to every key and value, without
a single cast.

You don't include this header yourself, because `cvector.h`, `chashmap.h`
and `cbstmap.h` each include it for you.

## A first example

```c
#include <stdio.h>
#include <ccollections/chashmap.h>

int main(void) {
    chmap_construct(stock, char *, int);
    chmap_insert(stock, "apples", 12);
    chmap_insert(stock, "pears", 0);
    chmap_insert(stock, "plums", 7);

    ccol_for_each(stock, it, {
        printf("%-8s %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
    });

    chmap_destroy(stock);
    return 0;
}
```

`ccol_for_each(container, it, { body })` declares the iterator `it`, runs
the body once for each element and destroys the iterator when the loop
stops, whether it reaches the end or you leave with `break` or `return`.
Inside the body `ccol_iter_key_ptr(it)` points to the current key and
`ccol_iter_val_ptr(it)` to the current value. Both pointers have the right
types, so in the example above `*ccol_iter_key_ptr(it)` is a `char *` and
`*ccol_iter_val_ptr(it)` is an `int`.

## What the key and the value are

| Container | `ccol_iter_key_ptr(it)` | `ccol_iter_val_ptr(it)` |
|---|---|---|
| `cvec` of `T` | `const size_t *`, the index | `T *`, the element |
| `chmap` of `K` to `V` | `const K *` | `V *` (`V const *` when `V` is `char *`) |
| `cbmap` of `K` to `V` | `const K *` | `V *` (`V const *` when `V` is `char *`) |

For a vector the "key" is the element's index, which means one loop gives
you both the index and the element:

```c
#include <stdio.h>
#include <ccollections/cvector.h>

int main(void) {
    cvec_construct(scores, int);
    cvec_push(scores, 95);
    cvec_push(scores, 82);
    cvec_push(scores, 78);

    ccol_for_each(scores, it, {
        printf("[%zu] = %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
    });

    cvec_destroy(scores);
    return 0;
}
```

## Order

A vector is visited by index from 0 upwards and a `cbmap` in ascending key
order. A `chmap` has no defined order at all, so don't depend on one; when
you need an order, copy the entries into a vector and sort it, as the word
counter example below does.

## Change values while you iterate

The value pointer points at the stored value itself, so you can change
values in place:

```c
ccol_for_each(prices, it, {
    *ccol_iter_val_ptr(it) *= 1.10;    /* a 10% increase */
});
```

Keys are read-only, and so is a `char *` value: the compiler won't let you
point it at another string. If you want to store a different string, insert
the key again after the loop.

What you must not do inside the loop is insert or remove elements of the
container you're walking, since that invalidates the iterator. Write down
the changes you want and apply them after the loop, like the example that
removes expired sessions further down.

## Write the loop yourself

For most loops `ccol_for_each` is the one to use, because it can't forget
to destroy the iterator. Write the `for` statement yourself only when you
want to keep the iterator after the loop or need a more complicated
condition. Declare the iterator with
`ccol_iter_declare` and move it with `ccol_begin` and `ccol_iter_next`:

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/cbstmap.h>

int main(void) {
    cbmap_construct(users, int, char *);
    cbmap_insert(users, 104, "dana");
    cbmap_insert(users, 101, "ada");
    cbmap_insert(users, 103, "chen");
    cbmap_insert(users, 102, "bob");

    ccol_iter_declare(users, it);
    for (it = ccol_begin(users); it != ccol_end; it = ccol_iter_next(it)) {
        if (strcmp(*ccol_iter_val_ptr(it), "chen") == 0)
            break;                      /* stop early */
        printf("%d %s\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
    }
    if (it)
        printf("found chen at id %d\n", *ccol_iter_key_ptr(it));
    ccol_iter_destroy(it);              /* safe on NULL too */

    cbmap_destroy(users);
    return 0;
}
```

`ccol_begin` returns `NULL` (`ccol_end`) for an empty container, and
`ccol_iter_next` returns `NULL` after the last element and frees the
iterator at that point. If you leave the loop early the iterator stays
alive, so call `ccol_iter_destroy(it)` once you're done with it, or let the
end of the scope free it, because `ccol_iter_declare` destroys it there by
itself.

## Iterate over a container that you received

The iterator macros need to know the container's types. A function that
gets a container as a parameter has to state those types again with the
container's `*_redeclare` macro first, the same as before any other typed
macro:

```c
#include <stdio.h>
#include <ccollections/chashmap.h>

static void print_fields(const char *title, chmap fields) {
    chmap_redeclare(fields, char *, char *);
    printf("%s:", title);
    ccol_for_each(fields, it, {
        printf(" %s=%s", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
    });
    printf("\n");
}

int main(void) {
    chmap_construct(shirt, char *, char *);
    chmap_insert(shirt, "color", "blue");
    chmap_insert(shirt, "size", "L");

    chmap_construct(mug, char *, char *);
    chmap_insert(mug, "material", "ceramic");

    print_fields("shirt", shirt);
    print_fields("mug", mug);

    chmap_destroy(shirt);
    chmap_destroy(mug);
    return 0;
}
```

The iterator treats a `NULL` container handle as an empty container, on
purpose. That helps with a container you only create when its first element
arrives, and the loop doesn't need a `NULL` check of its own.

## Example: the most frequent words

This program counts words with a hash map, copies the counts into a vector
while it walks the map, sorts the vector by count with the most frequent
word first, and prints the top three. The first sort puts the words in
alphabetical order and the sort is stable, so words with the same count
stay alphabetical.

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/chashmap.h>
#include <ccollections/cvector.h>

typedef struct {
    const char *word;                   /* points into the map */
    int count;
} WordCount;

static int by_word(const void *a, const void *b) {
    return strcmp(((const WordCount *)a)->word, ((const WordCount *)b)->word);
}

static int by_count_desc(const void *a, const void *b) {
    const WordCount *x = a, *y = b;
    return (y->count > x->count) - (y->count < x->count);
}

int main(void) {
    char text[] = "the cat and the dog and the bird saw a cat";

    chmap_construct(counts, char *, int);
    for (char *w = strtok(text, " "); w; w = strtok(NULL, " ")) {
        int *n = chmap_get_ptr(counts, w);
        if (n)
            (*n)++;
        else
            chmap_insert(counts, w, 1);
    }

    cvec_construct(table, WordCount);
    ccol_for_each(counts, it, {
        WordCount wc = {*ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it)};
        cvec_push(table, wc);
    });

    cvector_sort_with_comparison_proc(table, by_word);
    cvector_sort_with_comparison_proc(table, by_count_desc);

    for (size_t i = 0; i < 3 && i < cvec_size(table); i++)
        printf("%-5s %d\n", cvec_at(table, i).word, cvec_at(table, i).count);

    cvec_destroy(table);                /* before the map: it borrows keys */
    chmap_destroy(counts);
    return 0;
}
```

The `word` pointers in the table borrow the key copies that belong to the
map. That's fine here since the map doesn't change while the table is in
use, and the table is destroyed first.

## Example: remove expired sessions

A server keeps its sessions in an ordered map from a session id to the last
time it saw that session. Entries can't be removed while the loop walks the
map, so the function collects the expired ids in a vector during the loop
and removes them afterwards.

```c
#include <stdio.h>
#include <ccollections/cbstmap.h>
#include <ccollections/cvector.h>

static size_t remove_expired(cbmap sessions, long now, long max_idle) {
    cbmap_redeclare(sessions, int, long);

    cvec_construct_scoped(expired, int);
    ccol_for_each(sessions, it, {
        if (now - *ccol_iter_val_ptr(it) > max_idle)
            cvec_push(expired, *ccol_iter_key_ptr(it));
    });

    for (size_t i = 0; i < cvec_size(expired); i++)
        cbmap_remove(sessions, cvec_at(expired, i));
    return cvec_size(expired);
}

int main(void) {
    cbmap_construct(sessions, int, long);
    cbmap_insert(sessions, 7, 1000L);
    cbmap_insert(sessions, 3, 1590L);
    cbmap_insert(sessions, 9, 1200L);
    cbmap_insert(sessions, 5, 1700L);

    size_t gone = remove_expired(sessions, 1800L, 300L);
    printf("removed %zu\n", gone);
    ccol_for_each(sessions, it, {
        printf("session %d last seen %ld\n", *ccol_iter_key_ptr(it),
               *ccol_iter_val_ptr(it));
    });

    cbmap_destroy(sessions);
    return 0;
}
```

## Good to know

The key and value pointers are borrowed. They stay valid only while the
iterator is on that element and the container doesn't change, so copy
whatever you need to keep.

An iterator you left the loop with early can outlive its container. Once
the container is destroyed, don't move or read that iterator; the automatic
cleanup at the end of the scope is safe though.

Only `cvec`, `chmap` and `cbmap` work with these macros, and passing any
other type to `ccol_begin` or `ccol_for_each` doesn't compile. The macros
use statement expressions and the `cleanup` attribute, so you need GCC or
Clang with `-std=gnu11`.

## Reference

Overview: [citerators(7)](https://abunchofnerds-25.github.io/ccollections/man/citerators/citerators.7.html)

[ccol_for_each(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_for_each.3.html),
[ccol_iter_declare(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_iter_declare.3.html),
[ccol_begin(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_begin.3.html),
[ccol_end(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_end.3.html),
[ccol_iter_next(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_iter_next.3.html),
[ccol_iter_key_ptr(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_iter_key_ptr.3.html),
[ccol_iter_val_ptr(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_iter_val_ptr.3.html),
[ccol_iter_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_iter_destroy.3.html)

The per-container functions that `ccol_begin` calls:
[cvector_begin_iter(3)](https://abunchofnerds-25.github.io/ccollections/man/cvector/cvector_begin_iter.3.html),
[chashmap_begin_iter(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chashmap_begin_iter.3.html),
[cbmap_begin_iter(3)](https://abunchofnerds-25.github.io/ccollections/man/cbstmap/cbmap_begin_iter.3.html)

Related guides:
[Vectors](cvector.md),
[Hash maps](chashmap.md),
[Ordered maps](cbstmap.md),
[Design of the type-safe macros](design.md)
