# citerators: one loop for every container

`citerators` is the iteration API shared by the three containers that you
can iterate over: [cvector](cvector.md), [chashmap](chashmap.md) and
[cbstmap](cbstmap.md). You write the same loop for each of them and get typed
pointers to every key and value, with no casts.

Do not include this header yourself; `cvector.h`, `chashmap.h` and `cbstmap.h` each include it for you.

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

`ccol_for_each(container, it, { body })`:

1. declares the iterator `it`;
2. runs the body once for each element;
3. destroys the iterator when the loop stops, whether the loop reaches the
   end or you leave it with `break` or `return`.

Inside the body:

- `ccol_iter_key_ptr(it)` points to the current key.
- `ccol_iter_val_ptr(it)` points to the current value.

Both pointers have the correct types, so in the example above
`*ccol_iter_key_ptr(it)` is a `char *` and `*ccol_iter_val_ptr(it)` is an
`int`.

## What the key and the value are

| Container | `ccol_iter_key_ptr(it)` | `ccol_iter_val_ptr(it)` |
|---|---|---|
| `cvec` of `T` | `const size_t *`, the index | `T *`, the element |
| `chmap` of `K` to `V` | `const K *` | `V *` (`V const *` when `V` is `char *`) |
| `cbmap` of `K` to `V` | `const K *` | `V *` (`V const *` when `V` is `char *`) |

For a vector, the "key" is the index of the element, so one loop gives you
both the index and the element:

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

- A vector is visited by index, from 0 upwards.
- A `cbmap` is visited in ascending key order.
- A `chmap` has no defined order, so do not depend on one. When you need an
  order, copy the entries into a vector and sort them, as the word counter
  example below does.

## Change values while you iterate

The value pointer points at the stored value, so you can change values in
place:

```c
ccol_for_each(prices, it, {
    *ccol_iter_val_ptr(it) *= 1.10;    /* a 10% increase */
});
```

Keys are read-only, and so is a `char *` value: the compiler refuses to
make it point to a different string. To store a different string, insert the
key again after the loop.

Inside the loop, **do not insert or remove elements** of the container you
are iterating over, because that invalidates the iterator. Record the changes
you want to make and apply them after the loop, as the example below that
removes expired sessions does.

## Write the loop yourself

`ccol_for_each` covers most loops, but you can also write your own `for`
statement, for example to keep the iterator after the loop or to use a more
complex loop condition. Declare the iterator with `ccol_iter_declare`, then
move it with `ccol_begin` and `ccol_iter_next`:

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

The rules for this form are:

- `ccol_begin` returns `NULL` (`ccol_end`) when the container is empty.
- `ccol_iter_next` returns `NULL` after the last element, and frees the
  iterator at that point.
- If you leave the loop early, the iterator stays alive. Call
  `ccol_iter_destroy(it)` when you are done with it, or let the end of the
  scope free it, since `ccol_iter_declare` destroys it automatically there.

## Iterate over a container that you received

The iterator macros need to know the container's types. In a function that
receives a container as a parameter, restate the types first with the
container's `*_redeclare` macro, just as you would before any other typed
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

To the iterator, a `NULL` container handle is the same as an empty
container. This covers, for example, a container that you create only when
you add its first element, and it means the loop needs no `NULL` check of its
own.

## Example: the most frequent words

The program:

1. counts words with a hash map;
2. iterates over the map and copies the counts into a vector;
3. sorts the vector by count, most frequent word first;
4. prints the first three words.

Because the sort is stable and the first sort puts the words in alphabetical
order, words with the same count stay in alphabetical order.

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

The `word` pointers in the table borrow the key copies that the map owns.
That is safe here because the map does not change while the program uses the
table, and the program destroys the table first.

## Example: remove expired sessions

A server keeps its sessions in an ordered map from a session id to the last
time the server saw that session. Since entries must not be removed while the
loop iterates over the map, the function records the expired ids in a vector
during the loop and removes them afterwards.

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

- **Do not insert or remove elements while you iterate.** Record the
  changes and apply them after the loop.
- **The pointers are borrowed.** The key and value pointers are valid only
  while the iterator is on that element and the container does not change, so
  copy any data that you need to keep.
- **An iterator can outlive an early exit from the loop.** Once its container
  is destroyed, do not move or read that iterator; the automatic cleanup at
  the end of the scope is safe.
- **Only `cvec`, `chmap` and `cbmap` work.** Passing any other type to
  `ccol_begin` or `ccol_for_each` does not compile.
- **GNU C.** The macros use statement expressions and the `cleanup`
  attribute, so compile with GCC or Clang and `-std=gnu11`.

## Reference

Overview: [citerators(7)](../man/citerators/citerators.7)

[ccol_for_each(3)](../man/citerators/ccol_for_each.3),
[ccol_iter_declare(3)](../man/citerators/ccol_iter_declare.3),
[ccol_begin(3)](../man/citerators/ccol_begin.3),
[ccol_end(3)](../man/citerators/ccol_end.3),
[ccol_iter_next(3)](../man/citerators/ccol_iter_next.3),
[ccol_iter_key_ptr(3)](../man/citerators/ccol_iter_key_ptr.3),
[ccol_iter_val_ptr(3)](../man/citerators/ccol_iter_val_ptr.3),
[ccol_iter_destroy(3)](../man/citerators/ccol_iter_destroy.3)

The per-container functions that `ccol_begin` calls:
[cvector_begin_iter(3)](../man/cvector/cvector_begin_iter.3),
[chashmap_begin_iter(3)](../man/chashmap/chashmap_begin_iter.3),
[cbmap_begin_iter(3)](../man/cbstmap/cbmap_begin_iter.3)

Related guides:
[Vectors](cvector.md),
[Hash maps](chashmap.md),
[Ordered maps](cbstmap.md),
[Design of the type-safe macros](design.md)
