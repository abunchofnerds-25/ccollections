# chashmap: a hash map

`chashmap` maps keys to values. You give it a key and it gives you the
value stored for that key, in constant time on average however many entries
the map holds. It's the same idea as `std::unordered_map` in C++, `HashMap`
in Java or `dict` in Python.

You'd reach for a hash map to look things up by a name, an id or some other
key, to count or group items or drop duplicates, or to pick an action from
a string such as a command name, a field name or a route. If you need the
keys in sorted order, [cbstmap](cbstmap.md) is the better fit, and if a
plain array with a small integer index is enough, use
[cvector](cvector.md). When several threads share the map and you want the
module to do the locking, look at `clrucache`, a thread-safe cache built on
this map.

```c
#include <ccollections/chashmap.h>
```

## A first example

```c
#include <stdio.h>
#include <ccollections/chashmap.h>

int main(void) {
    chmap_construct(ages, char *, int);

    chmap_insert(ages, "alice", 31);
    chmap_insert(ages, "bob", 27);
    chmap_insert(ages, "alice", 32);          /* same key: updates the value */

    printf("alice is %d\n", chmap_get(ages, "alice"));
    printf("%zu people\n", chmap_elem_count(ages));

    int *carol = chmap_get_ptr(ages, "carol"); /* NULL when the key is absent */
    printf("carol is %s\n", carol ? "known" : "unknown");

    chmap_destroy(ages);
    return 0;
}
```

The macros use GNU C extensions, so compile with `-std=gnu11`, and link
with `-lccollections`:

```sh
gcc -std=gnu11 ages.c -lccollections -o ages
```

The program prints `alice is 32`, `2 people` and `carol is unknown`.

## Create a map

`chmap_construct(name, KeyType, ValueType)` declares a variable called
`name` and puts a new, empty map in it. Along with it come hidden companion
variables that record the two types, and that's how `chmap_insert` and the
other macros know how to copy, hash and return your keys and values. The
[design guide](design.md) explains this for the whole library.

Any C type works as a key or a value: integers, `float`, `double`,
`long double`, pointers, function pointers, structs and `char *` strings.
You don't pick the internal layout, the map picks it when you create it.
When both types are integers of eight bytes or less it uses a compact
open-addressing table with 17 bytes per slot (the key, the value and one
status byte), and for every other type it uses separate chaining. The API
is the same either way.

`chmap_destroy(name)` frees the map and sets `name` to `NULL`. If you want
the map freed automatically at the end of a block, use the scoped form:

```c
void handle_request(void) {
    chmap_construct_scoped(headers, char *, char *);
    /* ... use headers ... */
}   /* headers is destroyed here, on each path out of the block */
```

## Insert, find and remove keys

| You want to | Use |
|---|---|
| add a key, or replace its value | `chmap_insert(m, key, value)` |
| read a value you know is there | `chmap_get(m, key)` |
| check for a key, or change its value in place | `chmap_get_ptr(m, key)` |
| delete a key | `chmap_remove(m, key)` |
| count the entries | `chmap_elem_count(m)` |

`chmap_insert` is an "upsert": a new key is added and a key that is already
there gets its value replaced. `chmap_get` returns the value, but it stops
the program when the key isn't in the map, so only use it where a missing
key is a bug. `chmap_get_ptr` returns `NULL` for a missing key, and for a
key that is there it returns a pointer into the map that you can write
through:

```c
int *count = chmap_get_ptr(freq, word);
if (count)
    (*count)++;                 /* update in place, no second lookup */
else
    chmap_insert(freq, word, 1);
```

`chmap_remove` returns `ccol_success`, or `ccol_key_not_found` when the key
isn't in the map.

Keys and values are converted to the declared types the way a C assignment
converts them, so literals and computed expressions are fine. A `float` key
`1.5f` in a map of `int` becomes `1`, as in `int x = 1.5f;`. A struct
literal needs an extra pair of parentheses around it, since its commas
would split the macro arguments otherwise:
`chmap_insert(m, ((point){1, 2}), 7)`.

## Strings: the owner of each string

When the key type or the value type is `char *`, the map stores a copy of
the string and not your pointer. So you can reuse or free your buffer as
soon as `chmap_insert` returns, for example insert from a stack buffer that
you overwrite in a loop. A string the map gives you, from `chmap_get`,
`chmap_get_ptr` or an iterator, points into the map's own storage. Never
free it; it stays valid until the next change to the map (an insert, a
remove or a reset) or until the map is destroyed. To replace a stored
string with another one you insert the key again. For a string value
`chmap_get_ptr` gives you a `char *const *`, so you can change the
characters in place but not the pointer.

`char *`, `unsigned char *` and `signed char *` all mean a NUL-terminated
string in this library, and a `NULL` string pointer isn't a string at all.
`chmap_get_ptr` returns `NULL` for it, `chmap_remove` returns
`ccol_invalid_args`, and `chmap_insert` and `chmap_get` stop the program. If
you take a key from `getenv()` or a similar function, check it for `NULL`
first.

Every other pointer type is stored as the pointer itself. A map of `int` to
`struct user *` keeps the addresses you gave it and never frees the objects
behind them. Free them yourself before you destroy the map, or use
`chmap_destroy_with_dtor`, which calls a destructor on each value (the
inventory example below does this). The precise rules are in
[chashmap(7)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chashmap.7.html).

## Iterate over all entries

`ccol_for_each` visits every entry. `ccol_iter_key_ptr(it)` points to the
key, which is read-only, and `ccol_iter_val_ptr(it)` to the value, which you
can change in place:

```c
ccol_for_each(freq, it, {
    printf("%-12s %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});
```

A `break` or a `return` in the body is safe since the macro frees the
iterator for you, but don't insert or remove entries while you iterate. The
manual `ccol_begin` / `ccol_iter_next` form is described in the
[iterators guide](citerators.md).

The iteration order isn't defined. It can differ between two maps with the
same keys and between two runs of the same program, so for sorted output
use [cbstmap](cbstmap.md) or copy the keys into a vector and sort it.

## Give a map to a different function

A `chmap` is a handle and you pass it by value. The hidden type variables
don't travel with it, so the function that gets the map has to state the
types again with `chmap_redeclare`:

```c
void bump(chmap scores, const char *who) {
    chmap_redeclare(scores, char *, int);   /* same types as at creation */
    int *s = chmap_get_ptr(scores, who);
    if (s)
        (*s)++;
}
```

They must be the types you created the map with, and the compiler can't
check that for you.

## Example: count words

This program reads text from standard input and prints how many times each
word occurs. Every word is inserted from the same buffer, which gets
overwritten by the next word, and that's safe because the map copies each
string.

```c
#include <ctype.h>
#include <stdio.h>
#include <ccollections/chashmap.h>

/* Count the number of times that each word occurs on standard input. */
int main(void) {
    chmap_construct(freq, char *, int);

    char word[128];
    size_t len = 0;
    int c;
    do {
        c = getchar();
        if (c != EOF && isalpha(c)) {
            if (len < sizeof(word) - 1)
                word[len++] = (char)tolower(c);
            continue;
        }
        if (len == 0)
            continue;
        word[len] = '\0';
        len = 0;

        int *count = chmap_get_ptr(freq, word);
        if (count)
            (*count)++;                   /* update in place */
        else
            chmap_insert(freq, word, 1);  /* the map copies "word" */
    } while (c != EOF);

    ccol_for_each(freq, it, {
        printf("%6d  %s\n", *ccol_iter_val_ptr(it), *ccol_iter_key_ptr(it));
    });

    chmap_destroy(freq);
    return 0;
}
```

```sh
$ echo "the cat sat on the mat. The end" | ./words
```

It prints `the` (3) and `cat`, `sat`, `on`, `mat` and `end` (1) once each,
in no defined order.

## Example: a command dispatcher

A map from a command name to a function pointer replaces a long chain of
`strcmp` calls. For an unknown command `chmap_get_ptr` returns `NULL`, and
the program prints a message instead of crashing. The map is created in
`main` and used in two other functions, and each of them declares it again.

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/chashmap.h>

typedef void (*command_fn)(const char *arg);

static void cmd_look(const char *arg) { (void)arg; puts("A dusty room. A door to the north."); }
static void cmd_take(const char *arg) { printf("You take the %s.\n", arg ? arg : "nothing"); }
static void cmd_help(const char *arg) { (void)arg; puts("Commands: look, take <thing>, help, quit"); }

/* The map is passed from one function to another, so each function
   restates its types with chmap_redeclare. */
static void register_commands(chmap commands) {
    chmap_redeclare(commands, char *, command_fn);
    chmap_insert(commands, "look", cmd_look);
    chmap_insert(commands, "take", cmd_take);
    chmap_insert(commands, "help", cmd_help);
}

static int dispatch(chmap commands, char *line) {
    chmap_redeclare(commands, char *, command_fn);

    char *verb = strtok(line, " \t\n");
    char *arg = strtok(NULL, " \t\n");
    if (!verb)
        return 1;
    if (strcmp(verb, "quit") == 0)
        return 0;

    command_fn *fn = chmap_get_ptr(commands, verb);
    if (fn)
        (*fn)(arg);
    else
        printf("I do not know how to \"%s\".\n", verb);
    return 1;
}

int main(void) {
    chmap_construct(commands, char *, command_fn);
    register_commands(commands);

    char line[256];
    while (fputs("> ", stdout), fgets(line, sizeof(line), stdin))
        if (!dispatch(commands, line))
            break;

    chmap_destroy(commands);
    return 0;
}
```

## Struct keys

A struct can be a key. By default the map hashes and compares all the bytes
of the struct, padding included, which is right for a struct with no
padding, a fixed-size digest for example:

```c
typedef struct { unsigned char b[32]; } digest_t;

chmap_construct(seen, digest_t, int);
chmap_insert(seen, d, 1);            /* d is a digest_t */
```

A struct with padding, say a `char` followed by a `long`, is a trap with
that default, because its padding bytes take part in the hash and the
comparison. Such a struct needs `chmap_construct_full` with a hash function
and an equality function that read only the members. The two have to agree, which means keys that are
equal must have equal hashes, and the next example shows how.

An array can't be a key type or a value type. Declaring a map with `int[2]`
or `char[16]` is a compile error whose message tells you to wrap the array
in a struct like the `digest_t` above; C can't assign an array, so without
that error the map would store the address of your array instead of its
elements.

Floating-point keys are compared bit by bit, except that `-0.0` and `0.0`
are the same key. A NaN key is found by its exact bit pattern, and
`long double` keys are compared by value
([chmap_construct(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_construct.3.html) has the details).

Binary keys whose length is only known at run time, a slice of a buffer or
a byte string with zeros in it, can't go through the macros, because the
macros always treat a `char *` as a NUL-terminated string. Use the raw
layer described below for those.

## Example: an inventory with struct keys and owned values

The stock is keyed by (warehouse, sku). That key struct has padding, so the
map gets hash and equality functions that read each member. Each value is a
pointer to a heap record that owns a string, and `chmap_destroy_with_dtor`
frees all the records when the map goes away.

```c
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ccollections/chashmap.h>

/* A stock position is keyed by (warehouse, sku). The struct has padding
   after "warehouse", so the map gets a hash and an equality function that
   read only the members. */
typedef struct {
    char warehouse;
    long sku;
} stock_key;

typedef struct {
    char *description;   /* owned by the record */
    int quantity;
} stock_record;

static unsigned long stock_key_hash(const void *p, size_t size) {
    stock_key k;
    (void)size;
    memcpy(&k, p, sizeof(k));             /* p can have any alignment */
    return (unsigned long)k.sku * 31ul + (unsigned char)k.warehouse;
}

static bool stock_key_eq(const void *a, size_t as, const void *b, size_t bs) {
    stock_key x, y;
    (void)as; (void)bs;
    memcpy(&x, a, sizeof(x));
    memcpy(&y, b, sizeof(y));
    return x.warehouse == y.warehouse && x.sku == y.sku;
}

static stock_record *record_new(const char *description, int quantity) {
    stock_record *r = malloc(sizeof(*r));
    if (!r || !(r->description = strdup(description)))
        abort();                         /* out of memory */
    r->quantity = quantity;
    return r;
}

/* The map calls this once for each value when it is destroyed. */
static void record_free(const cmap_pair *val, void *ctx) {
    stock_record *r;
    (void)ctx;
    memcpy(&r, val->ptr, sizeof(r));      /* the value is the pointer itself */
    free(r->description);
    free(r);
}

int main(void) {
    chmap_construct_full(stock, stock_key, stock_record *, NULL,
                         stock_key_hash, stock_key_eq);

    stock_key k = {.warehouse = 'A', .sku = 1001};
    chmap_insert(stock, k, record_new("blue mug", 40));
    k = (stock_key){.warehouse = 'B', .sku = 1001};
    chmap_insert(stock, k, record_new("blue mug", 12));
    k = (stock_key){.warehouse = 'A', .sku = 2002};
    chmap_insert(stock, k, record_new("teapot", 3));

    /* Sell two teapots from warehouse A. */
    k = (stock_key){.warehouse = 'A', .sku = 2002};
    stock_record **r = chmap_get_ptr(stock, k);
    if (r)
        (*r)->quantity -= 2;

    ccol_for_each(stock, it, {
        const stock_key *key = ccol_iter_key_ptr(it);
        stock_record *rec = *ccol_iter_val_ptr(it);
        printf("%c/%ld  %-10s %d\n", key->warehouse, key->sku,
               rec->description, rec->quantity);
    });

    chmap_destroy_with_dtor(stock, record_free, NULL);
    return 0;
}
```

Your hash doesn't have to be a good one. The map mixes its result before
using it, so even an identity function or a small counter spreads well. The
function gets the address of the key and its stored size, which is
`strlen + 1` for a string and `sizeof` for a fixed-size type. If the
built-in equality is already right for your keys, `chmap_construct_ch`
takes only a hash function.

## When an error must not stop the program

The macros stop the program on a hard error such as running out of memory.
For most code that's the right default, but library code, a server or
anything else that has to recover should use the raw layer instead. When
`chmap_create` and its related functions fail they return `NULL` and an
error string, and every element function returns a `ccol_retval_t`. Keys
and values go to the raw layer as a `cmap_pair` (a pointer and a size), so
it also takes binary keys of any length. `chmap_get_elem_ref` fills a
`cmap_pair` that you own with the location and the size of the stored
value, and you can change the value bytes through its `ptr` until the next
insert, delete or resize of the map.

## Example: count fields without a copy

This program counts the distinct fields of a comma-separated line. Each key
is a slice of the line passed as a pointer and a length, so nothing is
copied out of the line, and every failure comes back as a return code.

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/chashmap.h>

/* Count the distinct fields of a comma-separated line without copying
   them: each key is a slice of the line, given to the raw layer as a
   pointer and a length. Every call returns a code, and none of them stops
   the program. */
int main(void) {
    const char line[] = "red,green,red,blue,green,red";
    char *err = NULL;

    chmap seen = chmap_create(16, ccol_other_types, ccol_int, &err);
    if (!seen) {
        fprintf(stderr, "cannot create map: %s\n", err);
        return 1;
    }

    const char *start = line;
    for (;;) {
        const char *end = strchr(start, ',');
        size_t len = end ? (size_t)(end - start) : strlen(start);

        cmap_pair key = {(void *)start, len};
        cmap_pair val;
        ccol_retval_t rc = chmap_get_elem_ref(seen, &key, &val);
        if (rc == ccol_success) {
            int count;
            memcpy(&count, val.ptr, sizeof(count));
            count++;
            memcpy(val.ptr, &count, sizeof(count));    /* update in place */
        } else if (rc == ccol_key_not_found) {
            int one = 1;
            cmap_pair v = {&one, sizeof(one)};
            rc = chmap_insert_elem(seen, &key, &v);
            if (rc != ccol_success) {
                fprintf(stderr, "insert failed: %d\n", (int)rc);
                break;
            }
        } else {
            fprintf(stderr, "lookup failed: %d\n", (int)rc);
            break;
        }

        if (!end)
            break;
        start = end + 1;
    }

    printf("%zu distinct values\n", chmap_elem_count(seen));
    cmap_pair red = {"red", 3};
    int n = 0;
    if (chmap_get_elem_copy(seen, &red, &n, sizeof(n)) == ccol_success)
        printf("red appears %d times\n", n);

    chmap_destroy(seen);
    return 0;
}
```

The output is `3 distinct values` and `red appears 3 times`. A key pointer
can have any alignment, which is why slices of a packed buffer work as keys.

## Keys from untrusted input

It's fine to give a map keys that an attacker picks, like the field names
of a JSON request. The map keeps an eye on how often its keys collide, and
once there are more collisions than random keys would cause, it switches to
a keyed hash with a secret value per process. A crafted set of keys can't
make the map slow that way, and you get this without doing anything.

The exception is a custom hash function. If your function returns the same
value for two keys, those keys collide no matter what the map does, so for
keys you don't trust use the built-in hash or a custom one that an attacker
can't make collide. The mechanism is described in
[chmap_create_full(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_create_full.3.html).

## Good to know

A pointer from `chmap_get_ptr`, a stored string or an iterator is valid only
until the next insert, remove or reset. A lookup never invalidates it.

The map frees its own copies of keys and values, but it never frees the
objects your non-string pointer values point to.

A map has no lock inside it. If more than one thread uses a map and any of
them changes it, protect every call with your own lock (see
[Concurrency](concurrency.md)).

## Reference

Overview: [chashmap(7)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chashmap.7.html)

Create and destroy (macros):
[chmap_construct(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_construct.3.html),
[chmap_construct_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_construct_scoped.3.html),
[chmap_construct_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_construct_mp.3.html),
[chmap_construct_mp_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_construct_mp_scoped.3.html),
[chmap_construct_ch(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_construct_ch.3.html),
[chmap_construct_ch_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_construct_ch_scoped.3.html),
[chmap_construct_full(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_construct_full.3.html),
[chmap_construct_full_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_construct_full_scoped.3.html),
[chmap_declare(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_declare.3.html),
[chmap_declare_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_declare_scoped.3.html),
[chmap_init(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_init.3.html),
[chmap_init_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_init_mp.3.html),
[chmap_init_ch(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_init_ch.3.html),
[chmap_init_full(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_init_full.3.html),
[chmap_redeclare(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_redeclare.3.html),
[chmap_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_destroy.3.html)

Element access (macros):
[chmap_insert(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_insert.3.html),
[chmap_get(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_get.3.html),
[chmap_get_ptr(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_get_ptr.3.html),
[chmap_remove(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_remove.3.html)

Raw functions:
[chmap_create(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_create.3.html),
[chmap_create_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_create_mp.3.html),
[chmap_create_ch(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_create_ch.3.html),
[chmap_create_full(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_create_full.3.html),
[chmap_insert_elem(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_insert_elem.3.html),
[chmap_get_elem_ref(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_get_elem_ref.3.html),
[chmap_get_elem_copy(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_get_elem_copy.3.html),
[chmap_delete_elem(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_delete_elem.3.html),
[chmap_elem_count(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_elem_count.3.html),
[chmap_reset(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_reset.3.html),
[chmap_destroy_with_dtor(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chmap_destroy_with_dtor.3.html),
[chashmap_begin_iter(3)](https://abunchofnerds-25.github.io/ccollections/man/chashmap/chashmap_begin_iter.3.html)

Iteration:
[ccol_for_each(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_for_each.3.html),
[ccol_begin(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_begin.3.html),
[ccol_iter_next(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_iter_next.3.html),
[ccol_iter_key_ptr(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_iter_key_ptr.3.html),
[ccol_iter_val_ptr(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_iter_val_ptr.3.html),
[ccol_iter_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/citerators/ccol_iter_destroy.3.html)

Related guides:
[cbstmap](cbstmap.md) (ordered map),
[Iterators](citerators.md),
[How the macros work](design.md),
[Memory management](memory.md) (the `_mp` variants),
[Concurrency](concurrency.md)
