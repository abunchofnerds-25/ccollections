# citerators: one loop for every container

`citerators` is the iteration API of the three containers that you can
iterate over: [cvector](cvector.md), [chashmap](chashmap.md) and
[cbstmap](cbstmap.md). You write the same loop for each container. You get
typed pointers to each key and value, without casts.

Do not include this header yourself. `cvector.h`, `chashmap.h` and
`cbstmap.h` each include it.

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

`ccol_for_each(container, it, { body })` does these steps:

1. It declares the iterator `it`.
2. It runs the body one time for each element.
3. It destroys the iterator when the loop stops. This occurs when the loop
   gets to the end. It also occurs when you go out of the loop with `break`
   or `return`.

In the body:

- `ccol_iter_key_ptr(it)` points to the current key.
- `ccol_iter_val_ptr(it)` points to the current value.

Both pointers have the correct types. Thus, in the example above,
`*ccol_iter_key_ptr(it)` is a `char *` and `*ccol_iter_val_ptr(it)` is an
`int`.

## What the key and the value are

| Container | `ccol_iter_key_ptr(it)` | `ccol_iter_val_ptr(it)` |
|---|---|---|
| `cvec` of `T` | `const size_t *`, the index | `T *`, the element |
| `chmap` of `K` to `V` | `const K *` | `V *` (`V const *` when `V` is `char *`) |
| `cbmap` of `K` to `V` | `const K *` | `V *` (`V const *` when `V` is `char *`) |

For a vector, the "key" is the index of the element. Thus one loop gives
you the index and the element:

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

- The iterator goes through a vector by index, from 0 up.
- The iterator goes through a `cbmap` in ascending key order.
- A `chmap` has no defined order. Do not depend on the order. When you need
  an order, copy the entries into a vector and sort them. The word counter
  example below shows this.

## Change values while you iterate

The value pointer points to the stored value. Thus you can change values
in place:

```c
ccol_for_each(prices, it, {
    *ccol_iter_val_ptr(it) *= 1.10;    /* a 10% increase */
});
```

Keys are read-only. A `char *` value is also read-only: you cannot make it
point to a different string, because the compiler refuses this. To keep a
different string, insert the key again after the loop.

In the loop, **do not insert or remove elements** of the container that you
iterate over. This makes the iterator invalid. Record the changes that you
want to make. Then make them after the loop. The example that removes
expired sessions, below, shows this procedure.

## Write the loop yourself

`ccol_for_each` is enough for most loops. You can also write your own
`for` statement. For example, you can keep the iterator after the loop, or
use a more complex loop condition. To do this, declare the iterator with
`ccol_iter_declare`. Then move it with `ccol_begin` and `ccol_iter_next`:

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

- `ccol_begin` gives `NULL` (`ccol_end`) when the container is empty.
- `ccol_iter_next` gives `NULL` after the last element. At that point, it
  also frees the iterator.
- If you go out of the loop early, the iterator continues to exist. Call
  `ccol_iter_destroy(it)` when you do not need it. Alternatively, let the end
  of the scope free it, because `ccol_iter_declare` destroys it
  automatically there.

## Iterate over a container that you received

The iterator macros must know the types of the container. In a function
that gets a container as a parameter, first state the types again. Use the
`*_redeclare` macro of the container, as you do before all other typed
macros:

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

A container handle that is `NULL` is the same as an empty container for
the iterator. An example is a container that you create only when you add
the first element. Thus the loop does not need its own `NULL` check.

## Example: the most frequent words

The program does these steps:

1. It counts words with a hash map.
2. It iterates over the map and copies the counts into a vector.
3. It sorts the vector by count, with the most frequent word first.
4. It prints the first three words.

The sort is stable, and the first sort puts the words in alphabetical
order. Thus words with the same count are in alphabetical order.

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

The `word` pointers in the table borrow the copies of the keys that the
map owns. This is correct here, because the map does not change while the
program uses the table. Also, the program destroys the table first.

## Example: remove expired sessions

A server keeps sessions in an ordered map. The map goes from a session id
to the last time that the server saw the session. You must not remove
entries while the loop iterates over the map. Thus the function records the
expired ids in a vector during the loop. After the loop, it removes them.

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

- **Do not insert or remove elements while you iterate.** First record the
  changes. Then make them after the loop.
- **The pointers are borrowed.** The key and value pointers are valid only
  while the iterator is on that element and the container does not change.
  Copy the data that you must keep.
- **An iterator can continue to exist after an early exit from the loop.**
  After its container is destroyed, do not move or read that iterator. The
  automatic cleanup at the end of the scope is safe.
- **Only `cvec`, `chmap` and `cbmap` work.** If you give a different type to
  `ccol_begin` or `ccol_for_each`, the code does not compile.
- **GNU C.** The macros use statement expressions and the `cleanup`
  attribute. Thus compile with GCC or Clang and `-std=gnu11`.

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

The functions of each container that `ccol_begin` calls:
[cvector_begin_iter(3)](../man/cvector/cvector_begin_iter.3),
[chashmap_begin_iter(3)](../man/chashmap/chashmap_begin_iter.3),
[cbmap_begin_iter(3)](../man/cbstmap/cbmap_begin_iter.3)

Related guides:
[Vectors](cvector.md),
[Hash maps](chashmap.md),
[Ordered maps](cbstmap.md),
[Design of the type-safe macros](design.md)
