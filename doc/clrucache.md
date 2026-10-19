# LRU cache: `clrucache`

A cache keeps the results of expensive work, so the next request for the
same input gets its answer right away. An LRU (least recently used) cache
has a fixed capacity, and when it's full and needs room it removes the
entry that nobody has read for the longest time. Results that callers keep
asking for stay in memory and old ones leave by themselves.

`clrucache` is a thread-safe LRU cache with O(1) lookup and O(1) eviction,
and it can also sit in front of a slower store. A remote getter fills the
cache on a miss (read-through), and threads that miss on the same key share
one fetch. A remote setter writes each update to the store first
(write-through). An eviction callback tells you about each entry that
leaves the cache.

It pays off when you look up the same keys many times, each lookup is
expensive (a database query, a file scan, a computation, a network call)
and memory use has to have a limit. If an entry must stay until you remove
it, you want a map instead, see [chashmap](chashmap.md).

```c
#include <ccollections/clrucache.h>
```

Link with `-lccollections -lpthread`. The typed macros use GNU C statement
expressions, so you need GCC or Clang (`-std=gnu11` or later).

## A first cache

```c
#include <ccollections/clrucache.h>
#include <stdio.h>

int main(void) {
    /* int keys, double values, a maximum of 3 entries; no callbacks */
    clru_construct(cache, int, double, 3, NULL, NULL, NULL);

    for (int k = 1; k <= 3; k++)
        clru_set(cache, k, k * 1.5);

    double out = 0.0;
    clru_get(cache, 1, &out);          /* reading key 1 makes it recent */
    clru_set(cache, 4, 6.0);           /* full: removes key 2, the oldest */

    for (int k = 1; k <= 4; k++) {
        if (clru_get(cache, k, &out) == ccol_success)
            printf("%d -> %.1f\n", k, out);
        else
            printf("%d -> (not cached)\n", k);
    }
    printf("size %zu of %zu\n", clrucache_size(cache),
           clrucache_capacity(cache));

    clru_destroy(cache);               /* cache becomes CLRU_CACHE_INVALID */
    return 0;
}
```

Output:

```
1 -> 1.5
2 -> (not cached)
3 -> 4.5
4 -> 6.0
size 3 of 3
```

`clru_construct(name, KeyT, ValT, capacity, getter, setter, evict_cb)`
declares the handle and creates the cache in one statement. Like the
library's other typed macros it records `KeyT` and `ValT`, and that lets
`clru_get` and `clru_set` convert your arguments for you ([How the library
is designed](design.md) explains how). The capacity has to be greater than
0. A cache of capacity 0 is refused, and `clru_construct` then stops the
process, as it does for any create that fails.

## The handle

A `clru_cache` is an opaque value and not a pointer. You can compare it
with `CLRU_CACHE_INVALID` or test it directly, since `CLRU_CACHE_INVALID` is
0 and `if (!cache)` means "no cache". Never cast it to or from a pointer.

Every handle is checked before it's used. A call on a destroyed handle
fails safely with `ccol_invalid_args`, and the size queries return 0 for it.
Destroying a handle twice stops the program with `abort()` rather than
freeing memory twice. `clru_destroy` waits for calls that other threads are
running, so you don't have to prove that nothing else is in progress.

## Getting values out

What `clru_get(cache, key, &out)` gives you depends on the value type. For
every value type except `char *`, the cache converts the stored value into
`out` the way a plain C assignment would. Nothing is allocated, so there's
nothing to free, and when the call fails `out` is left as it was. For a
`char *` value, `out` gets a new heap copy of the string that you own and
free with `free()` or with the free function of your custom allocator.
Each call makes its own copy, so two gets of the same key give you two
pointers to free.

The cache never hands out a pointer into its own storage, on purpose. A
copy costs a little, but it means an eviction on another thread can't remove
a value while you're using it.

The return value is `ccol_success`, `ccol_key_not_found`, or another error
code listed in [clru_get(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_get.3.html).
`ccol_key_not_found` means the key isn't in the cache and either there is
no getter or the getter failed.

### Keys

Any scalar type can be a key. `char *` works too and is compared as a
string, and a `char *` cache takes a `const char *` key or a string literal
without a cast.

An array such as `int[2]` or `char[16]` can't be a key type or a value
type. The declaring macros refuse it at compile time, because otherwise the
cache would store the address of your array, so wrap the array in a struct
instead.

A struct key is compared byte by byte, padding included. With GCC 11 or
later `clru_get` and `clru_set` clear the padding for you. With another
compiler, or with the raw functions, zero every byte of the key with
`memset` before you set its fields. "KEYS WITH PADDING" on
[clrucache_create_full(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clrucache_create_full.3.html) has the
details.

## Read-through: filling the cache on a miss

With a remote getter, a miss calls the getter to fetch the value. The
getter gets the key as a `cmap_pair` (`ptr` and `size`). It either
allocates the value on the heap, sets `val->ptr` and `val->size` and
returns `true`, or returns `false` when there is no value, and then the get
reports `ccol_key_not_found`.

The cache takes ownership of what the getter allocates and frees it later
with its own allocator, which is `free()` by default or the free function
of the allocator you gave at creation, so the getter has to allocate with
the matching function.

If several threads miss on the same key at the same time, only one of them
runs the getter while the others wait and get the same result.

```c
#include <ccollections/clrucache.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A simulated database: user id -> display name, 50 ms per query. */
static atomic_int queries;

static bool load_user(const cmap_pair *key, cmap_pair *val) {
    int id = *(const int *)key->ptr;
    atomic_fetch_add(&queries, 1);
    nanosleep(&(struct timespec){ .tv_nsec = 50 * 1000 * 1000 }, NULL);

    if (id <= 0 || id > 1000)
        return false;                       /* the user does not exist */

    char buf[32];
    snprintf(buf, sizeof(buf), "user-%04d", id);
    char *name = malloc(strlen(buf) + 1);   /* the cache frees it with free() */
    if (!name)
        return false;
    strcpy(name, buf);
    val->ptr = name;
    val->size = strlen(name) + 1;
    return true;
}

typedef struct {
    clru_cache users;
    int        id;
} request;

static void *request_handler(void *arg) {
    const request *req = arg;
    clru_cache users = req->users;          /* a plain local copy ... */
    clru_redeclare(users, int, char *);     /* ... gets its types again */
    int id = req->id;

    char *name = NULL;
    if (clru_get(users, id, &name) == ccol_success) {
        printf("thread asked for %d: %s\n", id, name);
        free(name);                         /* clru_get gave us a copy */
    } else {
        printf("thread asked for %d: unknown\n", id);
    }
    return NULL;
}

int main(void) {
    clru_construct(users, int, char *, 256, load_user, NULL, NULL);

    /* Eight threads ask at once for the same user, which is not cached. */
    request req = { users, 42 };
    pthread_t t[8];
    int started = 0;
    for (int i = 0; i < 8; i++)
        if (pthread_create(&t[i], NULL, request_handler, &req) == 0)
            started++;
    for (int i = 0; i < started; i++)
        pthread_join(t[i], NULL);

    /* A second request for the same user is a plain cache hit. */
    request_handler(&req);
    printf("database queries: %d\n", atomic_load(&queries));

    clru_destroy(users);
    return 0;
}
```

Nine requests make a single query. The eight misses that happen at the same
time share one fetch and the ninth request is a hit.

## Write-through and eviction callbacks

A remote setter runs before each update. When it returns `false`,
`clru_set` returns `ccol_unexpected_failure` and the cache doesn't touch
that key, so its value and its place in the eviction order stay the same.
When it returns `true`, the cache stores the new value and the key becomes
the most recently used one. The new value is copied before the setter is
called, so an out-of-memory result means the setter never ran, and a setter
that succeeds is always followed by a cache update that can't fail. That
way the cache never serves a value older than the one the store holds.

The eviction callback gets the key and the value of each entry the cache
removes to make room, and also of each entry left in the cache when you
destroy it.

```c
#include <ccollections/clrucache.h>
#include <stdio.h>

/* The store accepts only non-negative limits. */
static bool save_limit(const cmap_pair *key, const cmap_pair *val) {
    const char *name = key->ptr;
    long limit = *(const long *)val->ptr;
    if (limit < 0) {
        printf("  store: refused %s = %ld\n", name, limit);
        return false;
    }
    printf("  store: saved   %s = %ld\n", name, limit);
    return true;
}

/* Runs while the cache holds its internal lock: do not call the cache. */
static void on_evict(const cmap_pair *key, const cmap_pair *val) {
    printf("  evicted %s (was %ld)\n", (const char *)key->ptr,
           *(const long *)val->ptr);
}

int main(void) {
    clru_construct(limits, char *, long, 2, NULL, save_limit, on_evict);

    clru_set(limits, "max_connections", 100L);
    clru_set(limits, "max_body_kib", 512L);

    if (clru_set(limits, "max_connections", -1L) == ccol_unexpected_failure)
        printf("rejected; the cache still holds the old value\n");

    long v = 0;
    clru_get(limits, "max_connections", &v);
    printf("max_connections = %ld\n", v);

    /* Capacity 2: a third key evicts the least recently used one. */
    clru_set(limits, "idle_timeout_s", 30L);

    clru_destroy(limits);   /* also reports each entry in the cache */
    return 0;
}
```

Output:

```
  store: saved   max_connections = 100
  store: saved   max_body_kib = 512
  store: refused max_connections = -1
rejected; the cache still holds the old value
max_connections = 100
  store: saved   idle_timeout_s = 30
  evicted max_body_kib (was 512)
  evicted max_connections (was 100)
  evicted idle_timeout_s (was 30)
```

The getter and the setter run while the cache holds no lock, so they can
use the same cache for *other* keys. They must not get or set the key they
are serving, since the thread would then wait for itself, and they must not
destroy the cache. The eviction callback is different: it runs while the
cache holds a lock and must not call the cache at all. Evictions on two
threads can run it at the same time, so any state it keeps needs its own
protection.

Another thread can remove a key while that key's setter is running. Long
story short, the result is always the same as if the two operations had run
one after the other, and
[clrucache_set_full(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clrucache_set_full.3.html) goes through
this case in full.

## Passing a cache to other functions

`clru_get` and `clru_set` need the key and value types that
`clru_construct` recorded. In another function you call `clru_redeclare` to
record them again before the first get or set. It needs a plain variable
name, so a cache that lives in a struct gets copied into a local variable
first.

This small dictionary keeps its cache in an application struct and returns
string values that the caller frees:

```c
#include <ccollections/clrucache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simulates a slow scan of a large dictionary file. */
static const char *const dictionary[][2] = {
    { "cache",  "a store of things kept for later use" },
    { "evict",  "to expel from a property" },
    { "pool",   "a shared supply of something" },
    { "stride", "a long step" },
};
static int slow_lookups;

static bool load_definition(const cmap_pair *key, cmap_pair *val) {
    const char *word = key->ptr;
    slow_lookups++;
    for (size_t i = 0; i < sizeof(dictionary) / sizeof(dictionary[0]); i++) {
        if (strcmp(dictionary[i][0], word) == 0) {
            size_t n = strlen(dictionary[i][1]) + 1;
            char *copy = malloc(n);       /* the cache becomes the owner */
            if (!copy)
                return false;
            memcpy(copy, dictionary[i][1], n);
            val->ptr = copy;
            val->size = n;
            return true;
        }
    }
    return false;                         /* unknown word: not cached */
}

typedef struct {
    clru_cache definitions;               /* the 500 most recent words */
} dictionary_app;

static void lookup_word(dictionary_app *app, const char *word) {
    clru_cache defs = app->definitions;   /* redeclare needs a plain name */
    clru_redeclare(defs, char *, char *);

    char *definition = NULL;
    if (clru_get(defs, word, &definition) != ccol_success) {
        printf("%-7s (no definition)\n", word);
        return;
    }
    printf("%-7s %s\n", word, definition);
    free(definition);                     /* each clru_get returns a new copy */
}

int main(void) {
    clru_construct(defs, char *, char *, 500, load_definition, NULL, NULL);
    dictionary_app app = { defs };

    const char *session[] = { "pool", "cache", "pool", "llama", "cache",
                              "evict", "pool" };
    for (size_t i = 0; i < sizeof(session) / sizeof(session[0]); i++)
        lookup_word(&app, session[i]);

    printf("%zu lookups, %d slow ones\n", sizeof(session) / sizeof(session[0]),
           slow_lookups);
    clru_destroy(defs);
    return 0;
}
```

Seven lookups make four slow scans, one for each distinct word including
the unknown one. A failed fetch puts nothing into the cache, so asking for
the unknown word a second time scans again.

## Other ways to create a cache

`clru_declare` and `clru_init` split the declaration and the creation into
two steps, so you can create the cache later in a function for example.
`clru_construct_scoped` and `clru_declare_scoped` destroy the cache by
themselves when the variable goes out of scope.

The macros stop the program through `_ccol_fatal_err()` if creation fails.
If you want to handle that failure yourself, or give the cache a custom
allocator, call `clrucache_create_full()` and then `clru_redeclare` on the
handle:

```c
#include <ccollections/clrucache.h>
#include <stdio.h>

int main(void) {
    char *err = NULL;
    clru_cache prices = clrucache_create_full(
        1024,
        ccol_determine_ccol_data_type((int)0),      /* key type */
        ccol_determine_ccol_data_type((double)0),   /* value type */
        NULL, NULL, NULL,                           /* no callbacks */
        NULL,                                       /* default allocator */
        &err);
    if (prices == CLRU_CACHE_INVALID) {
        fprintf(stderr, "cache: %s\n", err);
        return 1;                                   /* no fatal error */
    }

    clru_redeclare(prices, int, double);            /* lets clru_get/set work */
    clru_set(prices, 7, 19.99);
    double p = 0;
    if (clru_get(prices, 7, &p) == ccol_success)
        printf("item 7 costs %.2f\n", p);

    clru_destroy(prices);
    return 0;
}
```

The allocator struct you pass is copied, so the struct itself may go away
after the call, but the functions it names have to stay valid until you
destroy the cache. Custom allocators, one backed by a memory pool among
them, are described in [Memory management](memory.md).

The raw functions `clrucache_get_full()` and `clrucache_set_full()` take
and return `cmap_pair` values directly, and a get through them always
returns a heap copy that you must free, whatever the value type.

## Concurrency and eviction order

Every operation is thread-safe. A cache with a capacity of 128 or more is
split into segments so the threads don't all wait on one lock. Each segment
has its own lock and its own share of the capacity, and a key always lives
in the same segment. Operations on the same key run one at a time, and
that's what makes the shared fetch and consistent reads work; a get of a
key waits for a set that is changing that key, for example.

Below a capacity of 128 there is a single segment, and the cache evicts in
exact least-recently-used order across all keys. From 128 up, each segment
evicts its own oldest entry when its share is full, which is the oldest in
its segment but not necessarily in the whole cache. A "full" cache also
holds a few percent fewer entries than its capacity, because the keys don't
spread over the segments in exactly equal numbers, although
`clrucache_capacity()` returns exactly the value you set. If your program
depends on the exact least-recently-used order, keep the capacity below
128.

`clrucache_size()` adds the segment sizes up one at a time, so while many
threads use the cache the result is approximate and not an exact snapshot
of one instant. Treat it as a figure for monitoring, and don't base a
decision on it while other threads are busy with the cache.

The `fork()` policy of the library applies. See [Concurrency](concurrency.md).

## Pitfalls

Every successful get of a string value returns a new heap copy, which you
free with the free function of the cache's allocator (`free()` by
default).

The getter has to allocate with the cache's allocator, because that's the
allocator the cache frees its result with.

Calling the cache from the eviction callback deadlocks, and so does getting
or setting the key that the getter or the setter is serving.

Each function that gets the handle needs `clru_redeclare`, or the typed
macros don't compile.

For a struct key with padding on a compiler other than GCC 11 or later,
use the raw functions and give them a key whose bytes are all zero.

`clru_destroy` sets your variable to `CLRU_CACHE_INVALID`, so destroying
that variable again does nothing, but destroying another copy of the same
handle afterwards stops the program.

## Reference

Overview: [clrucache(7)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clrucache.7.html)

Creating and destroying:

- [clru_construct(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_construct.3.html),
  [clru_construct_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_construct_scoped.3.html)
- [clru_declare(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_declare.3.html),
  [clru_declare_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_declare_scoped.3.html),
  [clru_init(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_init.3.html)
- [clru_redeclare(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_redeclare.3.html)
- [clru_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_destroy.3.html)
- [clrucache_create_full(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clrucache_create_full.3.html)

Reading and writing:

- [clru_get(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_get.3.html),
  [clru_set(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clru_set.3.html)
- [clrucache_get_full(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clrucache_get_full.3.html),
  [clrucache_set_full(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clrucache_set_full.3.html)

Inspecting:

- [clrucache_size(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clrucache_size.3.html),
  [clrucache_capacity(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clrucache_capacity.3.html)

Related guides:

- [How the library is designed](design.md): typed macros, lifecycle macros,
  `redeclare`, error handling and scoped variables
- [Memory management](memory.md): custom allocators
- [Concurrency](concurrency.md): thread safety and the `fork()` policy
- [chashmap](chashmap.md): a hash map with no size limit
