# cbstmap: an ordered map

`cbstmap` maps keys to values, as [chashmap](chashmap.md) does. It also
keeps the keys **in sorted order**. When you iterate over the map, you get
the entries from the smallest key to the largest key. The concept is the
same as `std::map` in C++ or `TreeMap` in Java.

The map is a self-balancing (AVL) tree. Thus an insert, a lookup and a
delete each take O(log n) time in the worst case, for all orders of keys.
Sorted input changes a simple binary tree into a linked list. Sorted input
does not make this map slow.

Use an ordered map when:

- you print, export or process entries in key order.
- you frequently need the smallest key (or the largest key). A scheduler or
  a leaderboard does this.
- you want a stable order that is the same for each run, and you do not
  want to sort after.

When the order is not important, use [chashmap](chashmap.md). On average,
its lookups take constant time, and they are faster.

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

Compile the program with `-std=gnu11`. Link it with `-lccollections`:

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

The API is the same as the API of the hash map, with `cbmap_` in place of
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

`cbmap_get` stops the program when the key is missing. For a missing key,
`cbmap_get_ptr` gives `NULL`. `cbmap_remove` gives `ccol_success` or
`ccol_key_not_found`. The macros convert keys and values to the declared
types in the same way as a C assignment. Thus you can give literals.

`cbmap_construct_scoped` destroys the map automatically at the end of the
block. A function that gets a `cbmap` must state its types again with
`cbmap_redeclare(m, KeyType, ValueType)`. These two macros work in the same
way as for the hash map and all other containers. The
[design guide](design.md) explains them.

## The order of the keys

For the usual key types, you do not have to do anything:

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

For all other key types, for example a struct, the map compares the raw
bytes, unless you give a comparison function. That byte order is almost
never the order that you want:

- It compares the padding bytes.
- It compares pointer members by address.
- On a little-endian machine, it puts `{1, 256}` before `{1, 2}`.

**Give a comparison function for each struct key.**

## A custom order

A comparison function gets pointers to two keys. It gives a negative
number, zero or a positive number, as the function that you give to `qsort`
does. Give it to the map with `cbmap_construct_cc`:

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

The program prints `0.9.1`, `1.2.3` and `1.10.0`, in that order. There are
three rules for a comparison function:

- **Zero identifies "the same key".** When you insert a key that is equal
  to a stored key, the map replaces the value of that entry. If the map must
  keep two different items, the function must not give zero for them. The
  leaderboard below compares the names when two scores are equal.
- **It must be consistent** for the full life of the map. It must be
  transitive. It must always give the same result for the same two keys.
- **Read keys with `memcpy`.** A key that comes through the raw functions
  can be at any address.

When you give a struct literal to a macro, put it in its own parentheses, as
in the example above. If you do not, its commas divide the macro
arguments.

## Strings and other pointers

The map copies a `char *` key or value into its storage. You can use your
buffer again, or free it, immediately after `cbmap_insert`. A string that the
map gives you points into the storage of the map. Never free it. Do not keep
it after the next insert, remove or reset. The map keeps a pointer value of
a different type as the pointer. The map never frees the memory that this
pointer points to. [cbstmap(7)](../man/cbstmap/cbstmap.7) gives all of these
rules.

## Example: a leaderboard

The program ranks the players by their best score, with the highest score
first. When two scores are equal, it uses the name. The rank key is a struct
with a custom order. Thus two players with the same score are two different
entries. A hash map goes from a name to the current best score. With this
map, an update can find and remove the old rank key of the player.

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/cbstmap.h>
#include <ccollections/chashmap.h>

/* A rank key: higher scores first. For equal scores, the name sets the
   order. Two players with the same score are two different keys. Thus the
   map keeps both players. */
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

/* "board" puts the players in order. "current" finds the current score
   of a player. Thus an update can first remove the old rank key. */
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

When you iterate over an ordered map, the smallest key is always the first
entry. Thus the map is a simple priority queue. In this example, the key of
each event is the millisecond at which the event must run. The program does
these steps:

1. It reads the first entry.
2. It stops the iteration.
3. It removes the entry.

The program removes the entry only after the iteration, because the map
must not change during an iteration.

```c
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ccollections/cbstmap.h>

/* A small scheduler. The key of each event is the millisecond at which
   the event must run. The smallest key is always the next event. */

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

The program copies out the event text before the remove. The map frees the
string that it gave together with its entry.

## When an error must not stop the program

The macros stop the program on a hard error, as the hash map macros do.
The raw functions report the error and do not stop the program:

- When `cbmap_create`, `cbmap_create_mp`, `cbmap_create_ch` and
  `cbmap_create_full` fail, they give `NULL` and an error string.
- `cbmap_insert_elem`, `cbmap_get_elem_ref`, `cbmap_get_elem_copy` and
  `cbmap_delete_elem` take each key and value as a `cmap_pair`. They give a
  `ccol_retval_t`.

The [chashmap guide](chashmap.md) shows a full program that uses the raw
layer. The calls are the same, with `cbmap_` names.

## Good to know

- **Do not change the map during an iteration.** First complete the loop,
  or go out of it with `break`. Then insert or remove, as the timer example
  does.
- **When the comparison gives zero, the map keeps one key only.** Make sure
  that your comparison does not give zero for keys that you must keep
  separately.
- **Struct keys need a comparison function.** The default byte order is
  almost never the order that you want.
- **Pointers into the map are valid for a short time only.** A pointer from
  `cbmap_get_ptr`, or a string that the map gives, is valid only until the
  next insert, remove or reset.
- **No locking.** If more than one thread uses a map, and one of the threads
  changes it, protect each call with your own lock. See
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
