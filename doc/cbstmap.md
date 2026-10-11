# cbstmap: an ordered map

`cbstmap` maps keys to values, as [chashmap](chashmap.md) does, but it also
keeps the keys **in sorted order**: when you iterate over the map, you get
the entries from the smallest key to the largest. The idea is the same as
`std::map` in C++ or `TreeMap` in Java.

The map is a self-balancing (AVL) tree, so an insert, a lookup and a delete
each take O(log n) time in the worst case, whatever the order of the keys.
Sorted input, which turns a simple binary tree into a linked list, does not
make this map slow.

Use an ordered map when:

- you print, export or process entries in key order;
- you often need the smallest (or the largest) key, as a scheduler or a
  leaderboard does;
- you want a stable order that is the same on every run, without sorting
  afterwards.

When the order does not matter, use [chashmap](chashmap.md) instead: its
lookups take constant time on average and are faster.

```c
#include <ccollections/cbstmap.h>
```

## A first example

```c
#include <stdio.h>
#include <ccollections/cbstmap.h>

int main(void) {
    cbmap_construct(scores, int, char *);

    cbmap_insert(scores, 91, "Diana");
    cbmap_insert(scores, 78, "Charlie");
    cbmap_insert(scores, 95, "Alice");
    cbmap_insert(scores, 82, "Bob");

    /* Iteration visits the keys from the smallest to the largest. */
    ccol_for_each(scores, it, {
        printf("%3d  %s\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
    });

    printf("who scored 95? %s\n", cbmap_get(scores, 95));
    cbmap_remove(scores, 78);
    printf("%zu entries left\n", cbmap_elem_count(scores));

    cbmap_destroy(scores);
    return 0;
}
```

Compile the program with `-std=gnu11` and link it with `-lccollections`:

```sh
gcc -std=gnu11 scores.c -lccollections -o scores
```

Output:

```
 78  Charlie
 82  Bob
 91  Diana
 95  Alice
who scored 95? Alice
3 entries left
```

## The basic operations

The API is the same as that of the hash map, with `cbmap_` in place of
`chmap_`:

| You want to | Use |
|---|---|
| create an empty map | `cbmap_construct(m, KeyType, ValueType)` |
| add a key, or replace its value | `cbmap_insert(m, key, value)` |
| read a value you know is there | `cbmap_get(m, key)` |
| check for a key, or change its value in place | `cbmap_get_ptr(m, key)` |
| delete a key | `cbmap_remove(m, key)` |
| count the entries | `cbmap_elem_count(m)` |
| empty the map and keep using it | `cbmap_reset(m)` |
| free the map | `cbmap_destroy(m)` |

`cbmap_get` stops the program when the key is missing, while
`cbmap_get_ptr` returns `NULL` for a missing key. `cbmap_remove` returns
`ccol_success` or `ccol_key_not_found`. The macros convert keys and values
to the declared types just as a C assignment does, so you can pass literals.

`cbmap_construct_scoped` destroys the map automatically at the end of the
block, and a function that receives a `cbmap` must state its types again with
`cbmap_redeclare(m, KeyType, ValueType)`. Both macros work the same way as
for the hash map and every other container; the
[design guide](design.md) explains them.

## The order of the keys

For the usual key types, there is nothing to do:

| Key type | Order |
|---|---|
| signed integers (`int`, `long`, `int8_t`, ...) | numeric, negatives first |
| unsigned integers, pointers | numeric |
| `char` | as `char` compares on your platform |
| `float`, `double`, `long double` | numeric; a NaN sorts after every number |
| `char *` | `strcmp` order |

```c
cbmap_construct(env, char *, char *);
cbmap_insert(env, "PATH", "/usr/bin");
cbmap_insert(env, "HOME", "/home/me");
cbmap_insert(env, "EDITOR", "vi");
/* an iteration gives EDITOR, HOME, PATH */
```

For any other key type, such as a struct, the map compares the raw bytes
unless you give it a comparison function. That byte order is almost never
the order you want, because:

- it compares the padding bytes;
- it compares pointer members by address;
- on a little-endian machine, it puts `{1, 256}` before `{1, 2}`.

**Give a comparison function for each struct key.**

## A custom order

A comparison function receives pointers to two keys and returns a
negative number, zero or a positive number, just like the function you pass
to `qsort`. Give it to the map with `cbmap_construct_cc`:

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/cbstmap.h>

typedef struct { int major, minor, patch; } version;

static int version_cmp(const void *a, const void *b) {
    version x, y;
    memcpy(&x, a, sizeof(x));
    memcpy(&y, b, sizeof(y));
    if (x.major != y.major) return (x.major > y.major) - (x.major < y.major);
    if (x.minor != y.minor) return (x.minor > y.minor) - (x.minor < y.minor);
    return (x.patch > y.patch) - (x.patch < y.patch);
}

int main(void) {
    cbmap_construct_cc(releases, version, char *, version_cmp);
    cbmap_insert(releases, ((version){1, 10, 0}), "faster parser");
    cbmap_insert(releases, ((version){1, 2, 3}), "bug fixes");
    cbmap_insert(releases, ((version){0, 9, 1}), "first preview");

    ccol_for_each(releases, it, {
        const version *v = ccol_iter_key_ptr(it);
        printf("%d.%d.%d  %s\n", v->major, v->minor, v->patch, *ccol_iter_val_ptr(it));
    });
    cbmap_destroy(releases);
    return 0;
}
```

The program prints `0.9.1`, `1.2.3` and `1.10.0`, in that order. A
comparison function must follow three rules:

- **Zero means "the same key".** When you insert a key that compares equal
  to a stored key, the map replaces the value of that entry, so if the map
  must keep two different items, the function must not return zero for
  them. The leaderboard below compares the names when two scores are equal.
- **It must be consistent** for the whole life of the map: transitive, and
  always returning the same result for the same two keys.
- **Read keys with `memcpy`,** because a key that comes through the raw
  functions can be at any address.

When you pass a struct literal to a macro, wrap it in its own parentheses,
as the example above does; otherwise its commas split the macro
arguments.

## Strings and other pointers

The map copies a `char *` key or value into its own storage, so you can
reuse or free your buffer as soon as `cbmap_insert` returns. A string that
the map gives you points into the map's storage: never free it, and do not
keep it after the next insert, remove or reset. A pointer value of any other
type is stored as the pointer itself, and the map never frees the memory it
points to. [cbstmap(7)](../man/cbstmap/cbstmap.7) lists all of these
rules.

## Example: a leaderboard

This program ranks players by their best score, highest first, and uses
the name to break a tie. The rank key is a struct with a custom order, so two
players with the same score are two different entries. A hash map from each
name to the current best score lets an update find and remove the player's
old rank key.

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/cbstmap.h>
#include <ccollections/chashmap.h>

/* A rank key: higher scores come first, and the name breaks a tie. Two
   players with the same score are two different keys, so the map keeps
   both of them. */
typedef struct {
    int score;
    char name[24];
} rank_key;

static int rank_cmp(const void *a, const void *b) {
    rank_key x, y;
    memcpy(&x, a, sizeof(x));
    memcpy(&y, b, sizeof(y));
    if (x.score != y.score)
        return (x.score < y.score) - (x.score > y.score);   /* descending */
    return strcmp(x.name, y.name);
}

static rank_key make_key(const char *name, int score) {
    rank_key k;
    memset(&k, 0, sizeof(k));
    k.score = score;
    snprintf(k.name, sizeof(k.name), "%s", name);
    return k;
}

/* "board" keeps the players in rank order, and "current" maps each player
   to their current score, so that an update can remove the old rank key
   first. */
static void record_score(cbmap board, chmap current, const char *name, int score) {
    cbmap_redeclare(board, rank_key, int);
    chmap_redeclare(current, char *, int);

    int *old = chmap_get_ptr(current, name);
    if (old) {
        if (*old >= score)
            return;                       /* keep the personal best */
        cbmap_remove(board, make_key(name, *old));
    }
    chmap_insert(current, name, score);
    cbmap_insert(board, make_key(name, score), 1);
}

static void print_top(cbmap board, int n) {
    cbmap_redeclare(board, rank_key, int);
    int rank = 0;
    ccol_for_each(board, it, {
        if (++rank > n)
            break;                        /* the macro frees the iterator */
        const rank_key *k = ccol_iter_key_ptr(it);
        printf("#%d  %-8s %5d\n", rank, k->name, k->score);
    });
}

int main(void) {
    cbmap_construct_cc(board, rank_key, int, rank_cmp);
    chmap_construct(current, char *, int);

    record_score(board, current, "ana", 1200);
    record_score(board, current, "ben", 900);
    record_score(board, current, "cem", 1200);
    record_score(board, current, "ben", 1500);  /* a higher best: ben goes up */
    record_score(board, current, "ana", 800);   /* not a best: no change */
    record_score(board, current, "dee", 400);

    print_top(board, 3);

    cbmap_destroy(board);
    chmap_destroy(current);
    return 0;
}
```

Output:

```
#1  ben       1500
#2  ana       1200
#3  cem       1200
```

## Example: a timer queue

Because iteration over an ordered map always starts at the smallest key,
the map works as a simple priority queue. In this example, the key of each
event is the millisecond at which the event must run, and the program takes
these steps:

1. It reads the first entry.
2. It stops the iteration.
3. It removes the entry.

It removes the entry only after the iteration, because the map must not
change while an iteration is in progress.

```c
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ccollections/cbstmap.h>

/* A small scheduler: each event is keyed by the millisecond at which it
   must run, so the smallest key is always the next event. */

static void schedule(cbmap timers, uint64_t due_ms, const char *what) {
    cbmap_redeclare(timers, uint64_t, char *);
    while (cbmap_get_ptr(timers, due_ms))   /* keep keys unique: add 1 ms */
        due_ms++;
    cbmap_insert(timers, due_ms, what);     /* the map copies the string */
}

/* Copy out the earliest event that must run at "now", and remove it. */
static bool pop_due(cbmap timers, uint64_t now, uint64_t *due, char *buf, size_t len) {
    cbmap_redeclare(timers, uint64_t, char *);
    bool found = false;
    ccol_for_each(timers, it, {
        if (*ccol_iter_key_ptr(it) <= now) {
            *due = *ccol_iter_key_ptr(it);
            snprintf(buf, len, "%s", *ccol_iter_val_ptr(it));
            found = true;
        }
        break;                              /* read only the first entry */
    });
    if (found)
        cbmap_remove(timers, *due);         /* change the map after the loop */
    return found;
}

int main(void) {
    cbmap_construct(timers, uint64_t, char *);

    schedule(timers, 250, "flush logs");
    schedule(timers, 100, "send heartbeat");
    schedule(timers, 100, "retry upload");  /* same time: goes to 101 */
    schedule(timers, 900, "rotate keys");

    char what[64];
    uint64_t due;
    for (uint64_t now = 0; now <= 1000; now += 200) {
        while (pop_due(timers, now, &due, what, sizeof(what)))
            printf("t=%4" PRIu64 "  ran \"%s\" (due %" PRIu64 ")\n", now, what, due);
    }
    printf("%zu timers left\n", cbmap_elem_count(timers));

    cbmap_destroy(timers);
    return 0;
}
```

Output:

```
t= 200  ran "send heartbeat" (due 100)
t= 200  ran "retry upload" (due 101)
t= 400  ran "flush logs" (due 250)
t=1000  ran "rotate keys" (due 900)
0 timers left
```

The program copies out the event text before the remove, because the map
frees the string it handed out together with its entry.

## When an error must not stop the program

Like the hash map macros, the macros stop the program on a hard error. The
raw functions report the error instead:

- `cbmap_create`, `cbmap_create_mp`, `cbmap_create_ch` and
  `cbmap_create_full` return `NULL` and an error string when they fail.
- `cbmap_insert_elem`, `cbmap_get_elem_ref`, `cbmap_get_elem_copy` and
  `cbmap_delete_elem` take each key and value as a `cmap_pair` and return a
  `ccol_retval_t`.

The [chashmap guide](chashmap.md) shows a complete program that uses the raw
layer; the calls are the same here, with `cbmap_` names.

## Good to know

- **Do not change the map during an iteration.** Finish the loop, or leave
  it with `break`, and then insert or remove, as the timer example does.
- **When the comparison returns zero, the map keeps only one key.** Make
  sure that your comparison does not return zero for keys that you must
  keep apart.
- **Struct keys need a comparison function,** because the default byte order
  is almost never the order you want.
- **Pointers into the map are short-lived.** A pointer from `cbmap_get_ptr`,
  or a string that the map gives you, is valid only until the next insert,
  remove or reset.
- **No locking.** If more than one thread uses a map and any of them changes
  it, protect every call with your own lock. See
  [Concurrency](concurrency.md).

## Reference

Overview: [cbstmap(7)](../man/cbstmap/cbstmap.7)

Create and destroy (macros):
[cbmap_construct(3)](../man/cbstmap/cbmap_construct.3),
[cbmap_construct_scoped(3)](../man/cbstmap/cbmap_construct_scoped.3),
[cbmap_construct_cc(3)](../man/cbstmap/cbmap_construct_cc.3),
[cbmap_construct_cc_scoped(3)](../man/cbstmap/cbmap_construct_cc_scoped.3),
[cbmap_construct_mp(3)](../man/cbstmap/cbmap_construct_mp.3),
[cbmap_construct_mp_scoped(3)](../man/cbstmap/cbmap_construct_mp_scoped.3),
[cbmap_construct_full(3)](../man/cbstmap/cbmap_construct_full.3),
[cbmap_construct_full_scoped(3)](../man/cbstmap/cbmap_construct_full_scoped.3),
[cbmap_declare(3)](../man/cbstmap/cbmap_declare.3),
[cbmap_declare_scoped(3)](../man/cbstmap/cbmap_declare_scoped.3),
[cbmap_init(3)](../man/cbstmap/cbmap_init.3),
[cbmap_init_cc(3)](../man/cbstmap/cbmap_init_cc.3),
[cbmap_init_mp(3)](../man/cbstmap/cbmap_init_mp.3),
[cbmap_init_full(3)](../man/cbstmap/cbmap_init_full.3),
[cbmap_redeclare(3)](../man/cbstmap/cbmap_redeclare.3),
[cbmap_destroy(3)](../man/cbstmap/cbmap_destroy.3)

Element access (macros):
[cbmap_insert(3)](../man/cbstmap/cbmap_insert.3),
[cbmap_get(3)](../man/cbstmap/cbmap_get.3),
[cbmap_get_ptr(3)](../man/cbstmap/cbmap_get_ptr.3),
[cbmap_remove(3)](../man/cbstmap/cbmap_remove.3)

Raw functions:
[cbmap_create(3)](../man/cbstmap/cbmap_create.3),
[cbmap_create_mp(3)](../man/cbstmap/cbmap_create_mp.3),
[cbmap_create_ch(3)](../man/cbstmap/cbmap_create_ch.3),
[cbmap_create_full(3)](../man/cbstmap/cbmap_create_full.3),
[cbmap_insert_elem(3)](../man/cbstmap/cbmap_insert_elem.3),
[cbmap_get_elem_ref(3)](../man/cbstmap/cbmap_get_elem_ref.3),
[cbmap_get_elem_copy(3)](../man/cbstmap/cbmap_get_elem_copy.3),
[cbmap_delete_elem(3)](../man/cbstmap/cbmap_delete_elem.3),
[cbmap_elem_count(3)](../man/cbstmap/cbmap_elem_count.3),
[cbmap_reset(3)](../man/cbstmap/cbmap_reset.3),
[cbmap_begin_iter(3)](../man/cbstmap/cbmap_begin_iter.3)

Iteration:
[ccol_for_each(3)](../man/citerators/ccol_for_each.3),
[ccol_begin(3)](../man/citerators/ccol_begin.3),
[ccol_iter_next(3)](../man/citerators/ccol_iter_next.3),
[ccol_iter_key_ptr(3)](../man/citerators/ccol_iter_key_ptr.3),
[ccol_iter_val_ptr(3)](../man/citerators/ccol_iter_val_ptr.3),
[ccol_iter_destroy(3)](../man/citerators/ccol_iter_destroy.3)

Related guides:
[chashmap](chashmap.md) (unordered map),
[Iterators](citerators.md),
[How the macros work](design.md),
[Memory management](memory.md) (the `_mp` variants),
[Concurrency](concurrency.md)
