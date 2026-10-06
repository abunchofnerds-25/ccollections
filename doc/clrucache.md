# LRU cache: `clrucache`

A cache keeps the results of work that costs much time. Therefore, the next
request for the same input gets its result immediately. An LRU (least recently
used) cache has a fixed capacity. When it is full and needs space, it removes
the entry that nobody read for the longest time. Results that callers ask for
frequently stay in memory. Old results go out of the cache automatically.

`clrucache` is a thread-safe LRU cache with O(1) lookup and O(1) eviction. It
can also operate in front of a slower store:

- a **remote getter** puts a value into the cache on a miss (read-through).
  Many threads that miss on the same key share one fetch;
- a **remote setter** writes each update to the store first (write-through);
- an **eviction callback** tells you about each entry that goes out of the
  cache.

**Use it when** these three conditions are true:

- you look for the same keys many times;
- the lookup costs much time (a database query, a file scan, a computation,
  a network call);
- the memory must have a limit.

**Do not** use it when each entry must stay until you remove it. For that, use a
map. See [chashmap](chashmap.md).

```c
#include <ccollections/clrucache.h>
```

Link with `-lccollections -lpthread`. The typed macros use GNU C statement
expressions. Therefore, build with GCC or Clang (`-std=gnu11` or later).

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
    clru_get(cache, 1, &out);          /* a read of key 1 makes it recent */
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
the handle and creates the cache in one statement. It records `KeyT` and `ValT`,
as the other typed macros of the library do. Therefore, `clru_get` and
`clru_set` can convert your arguments for you. For the method, see [How the
library is designed](design.md).

## The handle

A `clru_cache` is an opaque **value**, not a pointer. Compare it with
`CLRU_CACHE_INVALID`, or test it directly. `CLRU_CACHE_INVALID` is 0. Therefore,
`if (!cache)` means "no cache". Do not cast it to a pointer or from a pointer.

The library examines each handle before it uses it. A call on a destroyed
handle fails safely with `ccol_invalid_args`. The size queries return 0 in
this case. When you destroy a handle two times, the program stops with
`abort()`, and the library does not free memory two times. `clru_destroy`
waits for calls that other threads are running. Therefore, you do not have to
prove that no other call is in progress.

## Getting values out

The result of `clru_get(cache, key, &out)` depends on the value type:

- **All value types except `char *`**: the cache converts the stored value
  into `out`, as a plain C assignment does. The cache allocates nothing, and
  you have nothing to free. When the call fails, `out` does not change.
- **`char *` values**: `out` gets a new heap copy of the string, and you own it.
  Free it with `free()`, or with the free function of your custom allocator.
  Each call makes its own copy. Therefore, two gets of the same key give two
  pointers to free.

The cache does not give out a pointer into its own storage. Therefore, an
eviction on a different thread cannot remove a value that you use.

The return value is `ccol_success`, `ccol_key_not_found`, or a different
error code that [clru_get(3)](../man/clrucache/clru_get.3) lists.
`ccol_key_not_found` means that the key is not in the cache, and that there
is no getter or the getter failed.

### Keys

All scalar types are permitted as a key. `char *` is also permitted, and the
cache compares it as a string. A `char *` cache accepts a `const char *` key
or a string literal without a cast.

The cache compares a struct key byte by byte, with the padding. With GCC 11
or later, `clru_get` and `clru_set` clear the padding for you. With a
different compiler, or with the raw functions, set all bytes of the key to
zero with `memset` before you set its fields. For the details, see "KEYS WITH
PADDING" on
[clrucache_create_full(3)](../man/clrucache/clrucache_create_full.3).

## Read-through: filling the cache on a miss

Give the cache a remote getter. Then a miss calls the getter to fetch the
value. The getter gets the key as a `cmap_pair` (`ptr` and `size`). It must
do one of these two things:

- Allocate the value on the heap, set `val->ptr` and `val->size`, and return
  `true`.
- Return `false` when there is no value. The get then reports
  `ccol_key_not_found`.

The cache becomes the owner of the memory that the getter allocates. Later,
the cache frees it with its own allocator. That allocator is `free()` by
default, or the free function of the allocator that you gave at creation.
The getter must allocate with the function that agrees with it.

Several threads can miss on the same key at the same time. Then only one of
them runs the getter. The other threads wait and get the same result.

```c
#include <ccollections/clrucache.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A simulated database: user id -> display name, 50 ms for each query. */
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

    /* Eight threads ask at the same time for one user that is not cached. */
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

Nine requests cause one query. The eight misses at the same time share one
fetch, and the ninth request is a hit.

## Write-through and eviction callbacks

A remote setter runs before each update. When it returns `false`, `clru_set`
gives `ccol_unexpected_failure`. The cache then does not change for that
key: the value and the position in the eviction order stay the same. When
the setter returns `true`, the cache stores the new value, and the key
becomes the most recently used key.

The eviction callback gets the key and the value of each entry that the
cache removes to make space. It also gets each entry that is in the cache
when you destroy the cache.

```c
#include <ccollections/clrucache.h>
#include <stdio.h>

/* The store accepts only limits that are not negative. */
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

    /* Capacity 2: a third key removes the least recently used key. */
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

- **The getter and the setter** run while the cache holds no lock. Therefore,
  they can use the same cache for *other* keys. They must not get or set the key
  that they serve, because the thread would then wait for itself. They must not
  destroy the cache.
- **The eviction callback** runs while the cache holds a lock. It must not
  call the cache at all. Two evictions on different threads can run it at
  the same time. Therefore, the state that it keeps needs its own protection.

Another thread can remove a key while the setter of that key runs.
[clrucache_set_full(3)](../man/clrucache/clrucache_set_full.3) gives the
full description of this case. In summary, the result is always the same as
when the two operations run one after the other.

## Passing a cache to other functions

`clru_get` and `clru_set` need the key type and the value type that
`clru_construct` recorded. In a different function, call `clru_redeclare` to
record them again before the first get or set. It needs a plain variable
name. Therefore, copy a cache that is in a struct into a local variable first.

This small dictionary keeps its cache in an application struct. It gives
string values, and the caller frees them:

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
    free(definition);                     /* each clru_get gives a new copy */
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

Seven lookups cause four slow scans: one for each different word, and also one
for the unknown word. A failed fetch puts nothing into the cache. Therefore, a
second request for the unknown word causes one more scan.

## Other ways to create a cache

- `clru_declare` and `clru_init` do the declaration and the creation in two
  steps. For example, you can create the cache later in a function.
- `clru_construct_scoped` and `clru_declare_scoped` destroy the cache
  automatically when the variable goes out of its scope.
- The macros stop the program through `ccol_fatal_err()` if the creation
  fails. To manage that failure yourself, or to give the cache a custom
  allocator, call `clrucache_create_full()`. Then call `clru_redeclare` on
  the handle:

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

The cache copies the allocator struct that you give. Therefore, the struct
itself can stop existing after the call. The functions that it names must stay
valid until you destroy the cache. [Memory management](memory.md) describes
custom allocators, and also an allocator that uses a memory pool.

The raw functions `clrucache_get_full()` and `clrucache_set_full()` take and
return `cmap_pair` values directly. A get through them always gives a heap
copy that you must free, for all value types.

## Concurrency and eviction order

All operations are thread-safe. A cache with a capacity of 128 or more has
segments, so that threads do not wait on one lock. Each segment has its own
lock and its own part of the capacity. A key is always in the same segment.
Operations on the same key run one at a time. This makes the shared fetch
and the consistent reads possible. For example, a get of a key waits for a
set that changes that key.

The segments have two effects that you must know:

- **Below a capacity of 128**, the cache has one segment. It removes entries
  in the exact least-recently-used order across all keys.
- **At a capacity of 128 or more**, each segment removes its own oldest entry
  when its own part is full. The removed entry is the oldest entry of its
  segment, but possibly not of the full cache. Also, a "full" cache holds a few
  percent fewer entries than its capacity. The cause is that the keys do not go
  into the segments in exactly equal numbers. `clrucache_capacity()` gives
  exactly the value that you set.

`clrucache_size()` adds the sizes of the segments one by one. Therefore, when
many threads use the cache, the result is approximately correct, not exact for
one instant.

The `fork()` policy of the library applies. See [Concurrency](concurrency.md).

## Pitfalls

- **Free each `char *` value that `clru_get` gives you.** Each successful get
  of a string value gives you a new copy on the heap. Free it with the free
  function of the cache allocator, or with `free()` when the cache uses the
  default allocator.
- **Do not free a value that is not a string.** `out` is a plain variable, and
  there is nothing to free.
- **Allocate in the getter with the allocator of the cache.** The cache frees
  the memory that the getter gives with the allocator of the cache.
- **Do not call the cache from the eviction callback.** Also, do not get or
  set the key that the getter or the setter serves. Both cause a deadlock.
- **Use `clru_redeclare` in each function that gets the handle.** Without it,
  the typed macros do not compile.
- **Use the raw functions for a struct key with padding** when the compiler is
  not GCC 11 or later. Give them a key whose bytes are all zero.
- **Do not destroy a copy of a handle after a destroy.** `clru_destroy` sets
  your variable to `CLRU_CACHE_INVALID`, and a second destroy of that variable
  does nothing. A destroy of a different copy of the same handle stops the
  program.

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
