# chashmap: a hash map

`chashmap` maps keys to values. You give it a key. It gives you the value of
that key. On average, it does this in constant time, for all numbers of
entries in the map. The concept is the same as `std::unordered_map` in C++,
`HashMap` in Java or `dict` in Python.

Use a hash map when:

- you find items by a name, an id or a different key.
- you count, group or remove duplicate items.
- you select an action from a string, for example a command name, a field
  name or a route.

Use a different module in these conditions:

- You need the keys in sorted order. Use [cbstmap](cbstmap.md).
- A plain array with a small integer index is enough. Use
  [cvector](cvector.md).
- More than one thread uses the map, and you want the module to do the
  locking. `clrucache` is a thread-safe cache that uses this map.

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

Compile the program with `-std=gnu11`, because the macros use GNU C
extensions. Link it with `-lccollections`:

```sh
gcc -std=gnu11 ages.c -lccollections -o ages
```

The program prints `alice is 32`, `2 people` and `carol is unknown`.

## Create a map

`chmap_construct(name, KeyType, ValueType)` declares a variable with the
name `name`. It creates an empty map in that variable. It also declares
hidden companion variables that record the two types. With these types,
`chmap_insert` and the other macros know how to copy, hash and give back
your keys and values. The [design guide](design.md) explains this mechanism
for the full library.

All C types work as a key or a value. Examples are integers, `float`,
`double`, `long double`, pointers, function pointers, structs and `char *`
strings. You do not select the internal layout. The map selects one when you
create it:

- When both types are integers of eight bytes or less, the map uses a
  compact open-addressing table.
- For all other types, it uses separate chaining.

The API is the same for the two layouts.

`chmap_destroy(name)` frees the map and sets `name` to `NULL`. To free the
map automatically at the end of a block, use the scoped form:

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

`chmap_insert` is an "upsert". It adds a new key. For a key that is in the
map, it sets the new value. `chmap_get` gives the value. If the key is not in
the map, `chmap_get` stops the program. Thus use it only when a missing key
is a bug. `chmap_get_ptr` gives `NULL` for a missing key. For a key in the
map, it gives a pointer into the map. You can write through this pointer:

```c
int *count = chmap_get_ptr(freq, word);
if (count)
    (*count)++;                 /* update in place, no second lookup */
else
    chmap_insert(freq, word, 1);
```

`chmap_remove` gives `ccol_success`. When the key is not in the map, it
gives `ccol_key_not_found`.

The macros convert keys and values to the declared types, in the same way
as a C assignment. You can give literals and calculated expressions. For
example, a `float` key `1.5f` in a map of `int` becomes `1`, as in
`int x = 1.5f;`. When you give a struct literal as an argument, put it in an
added pair of parentheses. If you do not, its commas divide the macro
arguments: `chmap_insert(m, ((point){1, 2}), 7)`.

## Strings: the owner of each string

When the key type or the value type is `char *`, the map keeps a **copy of
the string**. It does not keep your pointer. This has three results:

- You can use your buffer again, or free it, immediately after
  `chmap_insert` returns. For example, you can insert from a stack buffer
  that you write over in a loop.
- A string that the map gives you points into the storage of the map. The
  string can come from `chmap_get`, `chmap_get_ptr` or an iterator. Never
  free it. It is valid until the next change to the map (an insert, a
  remove or a reset), or until the map is destroyed.
- To change a stored string to a different string, insert the key again.
  For a string value, `chmap_get_ptr` gives you a `char *const *`. You can
  change the characters in place. You cannot change the pointer.

For this library, `char *`, `unsigned char *` and `signed char *` each
identify a string with a terminating NUL. A `NULL` string pointer is not a
string:

- `chmap_get_ptr` gives `NULL` for it.
- `chmap_remove` gives `ccol_invalid_args`.
- `chmap_insert` and `chmap_get` stop the program.

Thus, before you use a value from `getenv()` or a similar function as a key,
make sure that it is not `NULL`.

**The map keeps a pointer of a different type as the pointer.** A map of
`int` to `struct user *` keeps the addresses that you gave it. It never frees
the objects at these addresses. Free them yourself before you destroy the
map. Alternatively, use `chmap_destroy_with_dtor`, which calls a destructor
on each value. The inventory example below shows this.
[chashmap(7)](../man/chashmap/chashmap.7) gives the precise rules.

## Iterate over all entries

`ccol_for_each` goes to each entry. `ccol_iter_key_ptr(it)` points to the
key, which is read-only. `ccol_iter_val_ptr(it)` points to the value. You can
change the value in place:

```c
ccol_for_each(freq, it, {
    printf("%-12s %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});
```

A `break` or a `return` in the body is safe. The macro frees the iterator
for you. Do not insert or remove entries while you iterate. The
[iterators guide](citerators.md) describes the manual `ccol_begin` /
`ccol_iter_next` form.

**The order of the iteration is not defined.** The order can be different
for two maps with the same keys. It can also be different for two runs of
the same program. When you need sorted output, use [cbstmap](cbstmap.md).
Alternatively, copy the keys into a vector and sort the vector.

## Give a map to a different function

A `chmap` is a handle. Thus you give it by value. The hidden type variables
do not go with the handle. Thus the function that gets the map must state
the types again with `chmap_redeclare`:

```c
void bump(chmap scores, const char *who) {
    chmap_redeclare(scores, char *, int);   /* same types as at creation */
    int *s = chmap_get_ptr(scores, who);
    if (s)
        (*s)++;
}
```

The types must be the types that you used to create the map. The compiler
cannot do this check for you.

## Example: count words

This program reads text from the standard input. It prints the number of
times that each word occurs. It inserts from a buffer that it writes over
for each word. This is safe, because the map copies each string.

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

This command prints each of `the` (3), `cat`, `sat`, `on`, `mat` and `end`
(1) one time. The order is not defined.

## Example: a command dispatcher

A map from a command name to a function pointer replaces a long sequence
of `strcmp` calls. `chmap_get_ptr` gives `NULL` for an unknown command. The
program then shows a message and does not crash. The program creates the
map in `main` and uses it in two other functions. Each of these functions
declares the map again.

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/chashmap.h>

typedef void (*command_fn)(const char *arg);

static void cmd_look(const char *arg) { (void)arg; puts("A dusty room. A door to the north."); }
static void cmd_take(const char *arg) { printf("You take the %s.\n", arg ? arg : "nothing"); }
static void cmd_help(const char *arg) { (void)arg; puts("Commands: look, take <thing>, help, quit"); }

/* The map goes from one function to a different function. Thus each
   function states the types again with chmap_redeclare. */
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

A struct can be a key. By default, the map hashes and compares **all the
bytes** of the struct. This includes the padding bytes. This is correct for
a struct with no padding, for example a fixed-size digest:

```c
typedef struct { unsigned char b[32]; } digest_t;

chmap_construct(seen, digest_t, int);
chmap_insert(seen, d, 1);            /* d is a digest_t */
```

For a struct with padding, for example a `char` and then a `long`, use
`chmap_construct_full`. Give it a hash function and an equality function
that read only the members. The two functions must agree: when two keys are
equal, their hashes must be equal. The next example shows this.

Two more facts about keys:

- **The map compares floating-point keys bit by bit**, with one exception:
  `-0.0` and `0.0` are the same key. The map finds a NaN key by its exact
  bit pattern. The map compares `long double` keys by value.
  [chmap_construct(3)](../man/chashmap/chmap_construct.3) gives the details.
- **You cannot give binary keys to the macros when you know their length
  only at run time.** Examples are a part of a buffer, or a byte string that
  contains zeros. The macros always use a `char *` as a string with a
  terminating NUL. Use the raw layer below.

## Example: an inventory with struct keys and owned values

The key of the stock is (warehouse, sku). The key struct has padding. Thus
the map gets hash and equality functions that read each member. Each value
is a pointer to a record on the heap, and the record owns a string. When the
map is destroyed, `chmap_destroy_with_dtor` frees each record.

```c
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ccollections/chashmap.h>

/* The key of a stock position is (warehouse, sku). The struct has padding
   after "warehouse". Thus the map gets a hash and an equality function that
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

/* The map calls this one time for each value when it is destroyed. */
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

A custom hash does not need to be a good hash. The map mixes the result of
your function before it uses it. Thus an identity function or a small
counter also gives a good distribution. Your function gets the address of
the key and its stored size. The size is `strlen + 1` for a string and
`sizeof` for a fixed-size type. When the built-in equality is correct for
your keys, use `chmap_construct_ch`. It takes only a hash function.

## When an error must not stop the program

The macros stop the program on a hard error, for example when there is no
more memory. This is the correct default for most code. Library code, a
server or other code that must recover must use the raw layer. When
`chmap_create` and the related functions fail, they give `NULL` and an error
string. Each element function gives a `ccol_retval_t`. Each key and value
goes to the raw layer as a `cmap_pair`, that is, a pointer and a size. Thus
the raw layer also accepts binary keys of any length.

## Example: count fields without a copy

This program counts the different fields in a line with comma separators.
Each key is a part of the line, which the program gives as a pointer and a
length. The program copies nothing out of the line. Each failure gives a
return code.

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/chashmap.h>

/* Count the different fields of a line with comma separators. Do not
   copy the fields. Each key is a part of the line, given to the raw layer
   as a pointer and a length. Each call gives a code. No call stops the
   program. */
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
        const cmap_pair *val = NULL;
        ccol_retval_t rc = chmap_get_elem_ref(seen, &key, &val);
        if (rc == ccol_success) {
            int count;
            memcpy(&count, val->ptr, sizeof(count));
            count++;
            memcpy(val->ptr, &count, sizeof(count));   /* update in place */
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

The program prints `3 distinct values` and `red appears 3 times`. A key
pointer can have any alignment. Thus you can use parts of a packed buffer as
keys.

## Keys from untrusted input

You can give a map keys that an attacker selects, for example the field
names of a JSON request. The map monitors the collisions of its keys. When
there are more collisions than random keys cause, the map changes to a
keyed hash. This hash uses a secret value for each process. Thus a special
set of keys cannot make the map slow. You do not have to do anything for
this.

The one exception is a custom hash function. When your function gives the
same value for two keys, these keys always collide. Thus, for keys that you
do not trust, use the built-in hash. Alternatively, use a custom hash that
an attacker cannot make collide.
[chmap_create_full(3)](../man/chashmap/chmap_create_full.3) describes the
mechanism.

## Good to know

- **Pointers into the map are valid for a short time only.** A pointer from
  `chmap_get_ptr`, a stored string or an iterator is valid only until the
  next insert, remove or reset. A lookup never makes it invalid.
- **The map does not own pointers that are not strings.** The map frees its
  copies of keys and values. It never frees the objects that your pointer
  values point to.
- **No locking.** A map has no internal lock. If more than one thread uses a
  map, and one of the threads changes it, protect each call with your own
  lock. See [Concurrency](concurrency.md).
- **The iteration order is not defined.** When you need a stable result,
  for example to print or to compare, sort the keys first.
- **`chmap_get` stops the program when the key is missing.** When a missing
  key is a normal result, use `chmap_get_ptr`.

## Reference

Overview: [chashmap(7)](../man/chashmap/chashmap.7)

Create and destroy (macros):
[chmap_construct(3)](../man/chashmap/chmap_construct.3),
[chmap_construct_scoped(3)](../man/chashmap/chmap_construct_scoped.3),
[chmap_construct_mp(3)](../man/chashmap/chmap_construct_mp.3),
[chmap_construct_mp_scoped(3)](../man/chashmap/chmap_construct_mp_scoped.3),
[chmap_construct_ch(3)](../man/chashmap/chmap_construct_ch.3),
[chmap_construct_ch_scoped(3)](../man/chashmap/chmap_construct_ch_scoped.3),
[chmap_construct_full(3)](../man/chashmap/chmap_construct_full.3),
[chmap_construct_full_scoped(3)](../man/chashmap/chmap_construct_full_scoped.3),
[chmap_declare(3)](../man/chashmap/chmap_declare.3),
[chmap_declare_scoped(3)](../man/chashmap/chmap_declare_scoped.3),
[chmap_init(3)](../man/chashmap/chmap_init.3),
[chmap_init_mp(3)](../man/chashmap/chmap_init_mp.3),
[chmap_init_ch(3)](../man/chashmap/chmap_init_ch.3),
[chmap_init_full(3)](../man/chashmap/chmap_init_full.3),
[chmap_redeclare(3)](../man/chashmap/chmap_redeclare.3),
[chmap_destroy(3)](../man/chashmap/chmap_destroy.3)

Element access (macros):
[chmap_insert(3)](../man/chashmap/chmap_insert.3),
[chmap_get(3)](../man/chashmap/chmap_get.3),
[chmap_get_ptr(3)](../man/chashmap/chmap_get_ptr.3),
[chmap_remove(3)](../man/chashmap/chmap_remove.3)

Raw functions:
[chmap_create(3)](../man/chashmap/chmap_create.3),
[chmap_create_mp(3)](../man/chashmap/chmap_create_mp.3),
[chmap_create_ch(3)](../man/chashmap/chmap_create_ch.3),
[chmap_create_full(3)](../man/chashmap/chmap_create_full.3),
[chmap_insert_elem(3)](../man/chashmap/chmap_insert_elem.3),
[chmap_get_elem_ref(3)](../man/chashmap/chmap_get_elem_ref.3),
[chmap_get_elem_copy(3)](../man/chashmap/chmap_get_elem_copy.3),
[chmap_delete_elem(3)](../man/chashmap/chmap_delete_elem.3),
[chmap_elem_count(3)](../man/chashmap/chmap_elem_count.3),
[chmap_reset(3)](../man/chashmap/chmap_reset.3),
[chmap_destroy_with_dtor(3)](../man/chashmap/chmap_destroy_with_dtor.3),
[chashmap_begin_iter(3)](../man/chashmap/chashmap_begin_iter.3)

Iteration:
[ccol_for_each(3)](../man/citerators/ccol_for_each.3),
[ccol_begin(3)](../man/citerators/ccol_begin.3),
[ccol_iter_next(3)](../man/citerators/ccol_iter_next.3),
[ccol_iter_key_ptr(3)](../man/citerators/ccol_iter_key_ptr.3),
[ccol_iter_val_ptr(3)](../man/citerators/ccol_iter_val_ptr.3),
[ccol_iter_destroy(3)](../man/citerators/ccol_iter_destroy.3)

Related guides:
[cbstmap](cbstmap.md) (ordered map),
[Iterators](citerators.md),
[How the macros work](design.md),
[Memory management](memory.md) (the `_mp` variants),
[Concurrency](concurrency.md)
