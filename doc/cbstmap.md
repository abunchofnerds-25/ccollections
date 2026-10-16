# cbstmap: an ordered map

`cbstmap` maps keys to values like [chashmap](chashmap.md) does, but it
also keeps the keys in sorted order, so iterating over the map gives you the
entries from the smallest key to the largest. It's the same idea as
`std::map` in C++ or `TreeMap` in Java.

Inside it is a self-balancing (AVL) tree, and an insert, a lookup and a
delete each take O(log n) in the worst case whatever order the keys arrive
in. Sorted input, which turns a plain binary tree into a linked list,
doesn't slow this map down.

You want an ordered map when you print, export or process entries in key
order, when you keep asking for the smallest (or largest) key the way a
scheduler or a leaderboard does, or when you need an order that is the same
on every run without sorting afterwards. If the order doesn't matter, use
[chashmap](chashmap.md) instead, since its lookups take constant time on
average and are faster.

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

Compile with `-std=gnu11` and link with `-lccollections`:

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

The API is the hash map's API with `cbmap_` in place of `chmap_`:

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

A missing key stops the program in `cbmap_get`, while `cbmap_get_ptr`
returns `NULL` for it. `cbmap_remove` returns `ccol_success` or
`ccol_key_not_found`. Keys and values are converted to the declared types
the way a C assignment converts them, so you can pass literals.

`cbmap_construct_scoped` destroys the map for you at the end of the
block, and a function that gets a `cbmap` as a parameter has to state its
types again with `cbmap_redeclare(m, KeyType, ValueType)`. Both behave as
they do for the hash map and every other container, and the
[design guide](design.md) explains them.

## The order of the keys

For the usual key types you don't have to do anything:

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

For any other key type, a struct for example, the map compares the raw
bytes unless you give it a comparison function. That is almost never the
order you want: it compares the padding bytes and compares pointer members
by address, and on a little-endian machine it puts `{1, 256}` before
`{1, 2}`. So give every struct key a comparison function.

## A custom order

A comparison function gets pointers to two keys and returns a negative
number, zero or a positive number, the same as the function you pass to
`qsort`. You hand it to the map with `cbmap_construct_cc`:

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

The program prints `0.9.1`, `1.2.3` and `1.10.0`, in that order.

Zero from the comparison means "the same key". If you insert a key that
compares equal to a stored one, the map replaces that entry's value, so
when two different items have to stay in the map the function must not
return zero for them (the leaderboard below compares the names when two
scores are equal). The function also has to be consistent for the whole
life of the map, which means transitive and always giving the same answer
for the same two keys. Read the keys with `memcpy`, because a key that
comes in through the raw functions can sit at any address.

A struct literal passed to a macro needs its own parentheses, as in the
example above, or its commas split the macro arguments.

## Strings and other pointers

A `char *` key or value is copied into the map's own storage, so you can
reuse or free your buffer as soon as `cbmap_insert` returns. A string the
map gives back points into that storage; never free it, and don't keep it
past the next insert, remove or reset. A pointer value of any other type is
stored as the pointer itself and the map never frees what it points to. All
of these rules are listed in [cbstmap(7)](../man/cbstmap/cbstmap.7).

## Example: a leaderboard

This program ranks players by their best score, highest first, with the
name breaking a tie. The rank key is a struct with a custom order, so two
players with the same score are two separate entries, and a hash map from
each name to the current best score lets an update find and remove the
player's old rank key.

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

Iteration over an ordered map always starts at the smallest key, so for a
small priority queue the map is a good fit. Here each event is keyed by
the millisecond at which it must run. The program reads the first entry,
stops the iteration and only then removes the entry, because the map must
not change while an iteration is running.

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

The event text is copied out before the remove, since the map frees the
string it handed out together with its entry.

## When an error must not stop the program

The macros stop the program on a hard error, just like the hash map macros,
and the raw functions report the error instead. `cbmap_create`,
`cbmap_create_mp`, `cbmap_create_ch` and `cbmap_create_full` return `NULL`
and an error string when they fail. `cbmap_insert_elem`,
`cbmap_get_elem_ref`, `cbmap_get_elem_copy` and `cbmap_delete_elem` take
each key and value as a `cmap_pair` and return a `ccol_retval_t`.
`cbmap_get_elem_ref` fills a `cmap_pair` that you own with the location and
the size of the stored value, and its `ptr` stays valid until the next
insert or delete.

A complete program that uses the raw layer is in the
[chashmap guide](chashmap.md); the calls are the same here with `cbmap_`
names.

## Good to know

Don't change the map during an iteration. Finish the loop or leave it with
`break`, then insert or remove, as the timer example does.

Struct keys need a comparison function, and that function must not return
zero for two keys you need to keep apart, because the map treats them as
one key (see [A custom order](#a-custom-order)).

An array can't be a key type or a value type. Declaring a map with
`int[2]` or `char[16]` is a compile error whose message tells you to wrap
the array in a struct; C can't assign an array, so without that error the
map would store the address of your array instead of its elements.

A pointer from `cbmap_get_ptr`, or a string the map gives you, is valid
only until the next insert, remove or reset.

There's no locking. If more than one thread uses a map and any of them
changes it, protect every call with your own lock (see
[Concurrency](concurrency.md)).

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
