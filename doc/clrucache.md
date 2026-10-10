# LRU cache: `clrucache`

A cache keeps the results of expensive work so that the next request for the
same input gets its answer immediately. An LRU (least recently used) cache has
a fixed capacity: when it is full and needs space, it removes the entry that
nobody has read for the longest time. Results that callers ask for often stay
in memory, while old results leave the cache on their own.

`clrucache` is a thread-safe LRU cache with O(1) lookup and O(1) eviction. It
can also sit in front of a slower store:

- a **remote getter** fills the cache on a miss (read-through), and many
  threads that miss on the same key share one fetch;
- a **remote setter** writes each update to the store first (write-through);
- an **eviction callback** tells you about each entry that leaves the cache.

**Use it when** all three of these are true:

- you look up the same keys many times;
- each lookup is expensive (a database query, a file scan, a computation,
  a network call);
- memory use must have a limit.

**Do not** use it when each entry must stay until you remove it; use a map
for that. See [chashmap](chashmap.md).

```c
#include <ccollections/clrucache.h>
```

Link with `-lccollections -lpthread`. The typed macros use GNU C statement
expressions, so build with GCC or Clang (`-std=gnu11` or later).

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

`clru_construct(name, KeyT, ValT, capacity, getter, setter, evict_cb)` declares
the handle and creates the cache in one statement. Like the other typed macros
of the library, it records `KeyT` and `ValT`, which lets `clru_get` and
`clru_set` convert your arguments for you. [How the library is
designed](design.md) explains the method.

## The handle

A `clru_cache` is an opaque **value**, not a pointer. Compare it with
`CLRU_CACHE_INVALID` or test it directly: `CLRU_CACHE_INVALID` is 0, so
`if (!cache)` means "no cache". Never cast it to or from a pointer.

The library checks each handle before it uses it. A call on a destroyed
handle fails safely with `ccol_invalid_args`, and the size queries return 0
in that case. Destroying a handle twice stops the program with `abort()`
instead of freeing memory twice. `clru_destroy` waits for calls that other
threads are running, so you do not have to prove that no other call is in
progress.

## Getting values out

What `clru_get(cache, key, &out)` gives you depends on the value type:

- **All value types except `char *`**: the cache converts the stored value
  into `out`, as a plain C assignment would. It allocates nothing, so you have
  nothing to free. When the call fails, `out` is left unchanged.
- **`char *` values**: `out` receives a new heap copy of the string, which you
  own; free it with `free()`, or with the free function of your custom
  allocator. Each call makes its own copy, so two gets of the same key give
  two pointers to free.

The cache never hands out a pointer into its own storage, so an eviction on
another thread cannot remove a value while you are using it.

The return value is `ccol_success`, `ccol_key_not_found`, or another error
code listed in [clru_get(3)](../man/clrucache/clru_get.3).
`ccol_key_not_found` means that the key is not in the cache and that either
there is no getter or the getter failed.

### Keys

Any scalar type can be a key. `char *` is allowed too, and the cache compares
it as a string; a `char *` cache accepts a `const char *` key or a string
literal without a cast.

The cache compares a struct key byte by byte, padding included. With GCC 11
or later, `clru_get` and `clru_set` clear the padding for you. With another
compiler, or with the raw functions, zero every byte of the key with `memset`
before you set its fields. For the details, see "KEYS WITH PADDING" on
[clrucache_create_full(3)](../man/clrucache/clrucache_create_full.3).

## Read-through: filling the cache on a miss

When you give the cache a remote getter, a miss calls the getter to fetch the
value. The getter receives the key as a `cmap_pair` (`ptr` and `size`) and
must do one of two things:

- allocate the value on the heap, set `val->ptr` and `val->size`, and return
  `true`;
- return `false` when there is no value, in which case the get reports
  `ccol_key_not_found`.

The cache takes ownership of the memory that the getter allocates and later
frees it with its own allocator: `free()` by default, or the free function of
the allocator you gave at creation. The getter must therefore allocate with
the matching function.

When several threads miss on the same key at the same time, only one of them
runs the getter; the others wait and receive the same result.

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

Nine requests cause a single query: the eight simultaneous misses share one
fetch, and the ninth request is a hit.

## Write-through and eviction callbacks

A remote setter runs before each update. If it returns `false`, `clru_set`
returns `ccol_unexpected_failure` and the cache leaves that key alone: both
its value and its position in the eviction order stay the same. If the setter
returns `true`, the cache stores the new value and the key becomes the most
recently used one.

The eviction callback receives the key and the value of each entry that the
cache removes to make space, and also of each entry that remains in the cache
when you destroy it.

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

The rules for the three callbacks:

- **The getter and the setter** run while the cache holds no lock, so they
  can use the same cache for *other* keys. They must not get or set the key
  they serve, because the thread would then wait for itself, and they must not
  destroy the cache.
- **The eviction callback** runs while the cache holds a lock, so it must not
  call the cache at all. Evictions on two different threads can run it at the
  same time, which means any state it keeps needs its own protection.

Another thread can remove a key while that key's setter is running.
[clrucache_set_full(3)](../man/clrucache/clrucache_set_full.3) describes
this case in full; in short, the result is always the same as if the two
operations had run one after the other.

## Passing a cache to other functions

`clru_get` and `clru_set` need the key and value types that `clru_construct`
recorded. In another function, call `clru_redeclare` to record them again
before the first get or set. Because it needs a plain variable name, copy a
cache that lives in a struct into a local variable first.

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

Seven lookups cause four slow scans: one for each distinct word, including
the unknown one. A failed fetch puts nothing into the cache, so a second
request for the unknown word causes another scan.

## Other ways to create a cache

- `clru_declare` and `clru_init` split the declaration and the creation into
  two steps, for example so that you can create the cache later in a
  function.
- `clru_construct_scoped` and `clru_declare_scoped` destroy the cache
  automatically when the variable goes out of scope.
- The macros stop the program through `ccol_fatal_err()` if creation fails.
  To handle that failure yourself, or to give the cache a custom allocator,
  call `clrucache_create_full()` and then `clru_redeclare` on the handle:

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

The cache copies the allocator struct you pass, so the struct itself may go
away after the call, but the functions it names must stay valid until you
destroy the cache. [Memory management](memory.md) describes custom
allocators, including one backed by a memory pool.

The raw functions `clrucache_get_full()` and `clrucache_set_full()` take and
return `cmap_pair` values directly. A get through them always returns a heap
copy that you must free, whatever the value type.

## Concurrency and eviction order

All operations are thread-safe. A cache with a capacity of 128 or more is
split into segments so that threads do not all wait on one lock; each segment
has its own lock and its own share of the capacity, and a key always lives in
the same segment. Operations on the same key run one at a time, which is what
makes the shared fetch and consistent reads possible: for example, a get of a
key waits for a set that is changing that key.

Segmentation has two effects that you need to know about:

- **Below a capacity of 128**, the cache has a single segment and evicts in
  exact least-recently-used order across all keys.
- **At a capacity of 128 or more**, each segment evicts its own oldest entry
  when its share is full. The evicted entry is the oldest in its segment, but
  not necessarily in the whole cache. A "full" cache also holds a few percent
  fewer entries than its capacity, because keys do not spread across the
  segments in exactly equal numbers. `clrucache_capacity()`, however, returns
  exactly the value you set.

`clrucache_size()` adds up the segment sizes one at a time, so while many
threads use the cache the result is approximate rather than an exact snapshot
of one instant.

The `fork()` policy of the library applies. See [Concurrency](concurrency.md).

## Pitfalls

- **Free each `char *` value that `clru_get` gives you.** Every successful get
  of a string value returns a new heap copy. Free it with the free function of
  the cache's allocator, or with `free()` when the cache uses the default
  allocator.
- **Do not free a value that is not a string.** `out` is a plain variable, so
  there is nothing to free.
- **Allocate in the getter with the cache's allocator,** because that is the
  allocator the cache uses to free what the getter returns.
- **Do not call the cache from the eviction callback,** and do not get or set
  the key that the getter or the setter is serving; both cause a deadlock.
- **Use `clru_redeclare` in each function that receives the handle.** Without
  it, the typed macros do not compile.
- **Use the raw functions for a struct key with padding** when the compiler is
  not GCC 11 or later, and give them a key whose bytes are all zero.
- **Do not destroy another copy of a handle after a destroy.** `clru_destroy`
  sets your variable to `CLRU_CACHE_INVALID`, so a second destroy of that
  variable does nothing, but destroying a different copy of the same handle
  stops the program.

## Reference

Creating and destroying:

- [clru_construct(3)](../man/clrucache/clru_construct.3),
  [clru_construct_scoped(3)](../man/clrucache/clru_construct_scoped.3)
- [clru_declare(3)](../man/clrucache/clru_declare.3),
  [clru_declare_scoped(3)](../man/clrucache/clru_declare_scoped.3),
  [clru_init(3)](../man/clrucache/clru_init.3)
- [clru_redeclare(3)](../man/clrucache/clru_redeclare.3)
- [clru_destroy(3)](../man/clrucache/clru_destroy.3)
- [clrucache_create_full(3)](../man/clrucache/clrucache_create_full.3)

Reading and writing:

- [clru_get(3)](../man/clrucache/clru_get.3),
  [clru_set(3)](../man/clrucache/clru_set.3)
- [clrucache_get_full(3)](../man/clrucache/clrucache_get_full.3),
  [clrucache_set_full(3)](../man/clrucache/clrucache_set_full.3)

Inspecting:

- [clrucache_size(3)](../man/clrucache/clrucache_size.3),
  [clrucache_capacity(3)](../man/clrucache/clrucache_capacity.3)

Related guides:

- [How the library is designed](design.md): typed macros, lifecycle macros,
  `redeclare`, error handling and scoped variables
- [Memory management](memory.md): custom allocators
- [Concurrency](concurrency.md): thread safety and the `fork()` policy
- [chashmap](chashmap.md): a hash map with no size limit
