# Memory management

Every allocation that c_collections makes goes through a table of four
functions, and you can replace them with your own. This guide shows how you
plug in your own allocator, what it has to promise and how long it has to
stay around, and it ends with two realistic allocators, a per-request arena
and a memory pool.

You don't need any of this to use the library. If you pass nothing (or
`NULL`), each module uses the standard `malloc`, `calloc`, `realloc` and
`free`.

## The allocator table

```c
typedef struct ccol_memmgmt_procs_t {
    ccol_malloc_t  malloc;    /* void *(*)(size_t)          */
    ccol_free_t    free;      /* void  (*)(void *)          */
    ccol_calloc_t  calloc;    /* void *(*)(size_t, size_t)  */
    ccol_realloc_t realloc;   /* void *(*)(void *, size_t)  */
} ccol_memmgmt_procs_t;
```

Every module that allocates memory takes a pointer to such a table when it
creates an object:

| Module | Form that takes a table |
|---|---|
| `cvector`, `chashmap`, `cbstmap`, `cstring` | `cvec_construct_mp`, `chmap_construct_mp`, `cbmap_construct_mp`, `cstr_construct_mp` (and the related `_init_mp`, `_scoped` and `_full` forms) |
| `cjson`, `cyaml` | `cjson_parse_mp`, `cjson_create_*_mp`, and the same forms for `cyaml` |
| `cthreadcomm` | `ccol_circular_queue_create_mp` and the other `_create_mp` constructors |
| `cmempool` | an argument of `ccol_mempool_create` and `ccol_r_mempool_create` |
| `clrucache`, `clogger`, `cthreadpool`, `chttpclient`, `chttpserver` | `clrucache_create_full`, `clog_create_from_file_mp`, `ctpool_create_mp`, `chttpclient_create_mp`, `chttpsvr_create_mp` |

A form without `_mp` is the same call with `NULL`.

An object frees all its memory through the table it got when it was
created, and that's why destroy calls never take an allocator argument.

## A first custom allocator

This allocator passes each call on to the standard functions and counts
the calls, so you can check that your program gives back all the memory it
takes:

```c
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <ccollections/chashmap.h>
#include <ccollections/cvector.h>

/* A counting allocator: the standard functions plus two counters. The
 * counters are atomic, so thread-safe objects can use the table too. */
static atomic_size_t live_blocks, total_calls;

static void *count_malloc(size_t n) {
    void *p = malloc(n);
    if (p) { atomic_fetch_add(&live_blocks, 1); atomic_fetch_add(&total_calls, 1); }
    return p;
}
static void *count_calloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (p) { atomic_fetch_add(&live_blocks, 1); atomic_fetch_add(&total_calls, 1); }
    return p;
}
static void *count_realloc(void *old, size_t n) {
    void *p = realloc(old, n);
    if (p) {
        if (!old) atomic_fetch_add(&live_blocks, 1);
        atomic_fetch_add(&total_calls, 1);
    }
    return p;
}
static void count_free(void *p) {
    if (p) atomic_fetch_sub(&live_blocks, 1);
    free(p);
}

static ccol_memmgmt_procs_t counting = {
    .malloc = count_malloc, .free = count_free,
    .calloc = count_calloc, .realloc = count_realloc,
};

int main(void) {
    cvec_construct_mp(v, double, &counting);
    chmap_construct_mp(m, char *, int, &counting);

    for (int i = 0; i < 1000; i++)
        cvec_push(v, i * 0.5);
    chmap_insert(m, "answer", 42);

    printf("while alive: %zu blocks, %zu calls\n",
           atomic_load(&live_blocks), atomic_load(&total_calls));

    chmap_destroy(m);
    cvec_destroy(v);
    printf("after destroy: %zu blocks\n", atomic_load(&live_blocks));
    return atomic_load(&live_blocks) == 0 ? 0 : 1;
}
```

```
while alive: 5 blocks, 13 calls
after destroy: 0 blocks
```

(The exact counts can change between versions of the library, but the
block count always goes back to zero.)

## What your functions must promise

Your functions have to behave exactly like the standard ones. All four
pointers must be non-NULL, and `malloc(0)`, `realloc(NULL, n)`, `free(NULL)`
and a failed allocation (which gives NULL) must work the way they do in the
C library.

That covers the alignment too. Every block from `malloc`, `calloc` and
`realloc` must be aligned for any object type, which means
`_Alignof(max_align_t)` (16 bytes on x86-64 and aarch64), because the
containers put entries that need this alignment at the start of a block. An
arena or a pool that hands out smaller pieces has to round each piece up.

When the object is thread-safe, your functions have to be as well. A queue,
a thread pool, a logger or a cache calls your functions from whatever
thread does the work, so only give such an object a table whose functions
are safe for concurrent calls.

## How long the allocator must live

For every module except `cmempool`, the last call to your functions
happens before the object's destroy returns. So the functions have to keep
working until you have destroyed every object that uses them, and the table
itself (the `ccol_memmgmt_procs_t` struct) has to stay valid for the same
time unless the module keeps its own copy. `clrucache`, `cjson` and `cyaml`
do keep a copy, so with them you can release the struct as soon as the call
that creates the object returns. A `static` table like the ones in these
examples is the easy way out.

A `cmempool` pool that threads share is an exception. Such a pool keeps a
small cache for each thread that uses it, and when you destroy the pool it
marks those caches as dead, but it can't free them while their threads can
still use them. The library frees each cache later instead, when its thread
next needs a cache for a different pool, when that thread exits, or when
the process exits. A thread that does none of these can keep a dead cache
for the whole life of the process, which means the pool's allocator can run
after `ccol_mempool_destroy` returns, and even after `main` returns. That
only matters for an allocator that stops working at a known point, like an
arena that you free at the end of `main`. For such a pool, either give it an
allocator that lives as long as the process, or create it with
`single_threaded` set so it keeps no per-thread caches, or join the threads
that used it.

The engine of the HTTP client is the second exception. The client runs
Tiers 2 and 3 on one engine for the whole process, and
`chttpcli_set_engine_mem_mgmt_procs()` gives that engine your allocator.
The engine stops on a thread of its own after the last client that used
those tiers lets go of it, so it can call your functions after
`chttpclient_destroy()` returns. Keep them working until
`chttpcli_engine_wait()` returns after you destroy the last client (that
call blocks until the engine has stopped). The default client of
`chttp_default_client()` can hold the engine through its idle connections
until the library's exit handler destroys it, so a process that uses the
default client for Tiers 2 and 3 must keep the engine allocator working
until it exits.

The engine logger of `chttpcli_set_engine_logger()` and the allocator of
the logger you give it follow the same rule, because the engine writes
through that logger while it runs. Once `chttpcli_engine_wait()` has
returned, `chttpcli_set_engine_logger(CLOG_INVALID)` releases it and the
engine makes no more calls through it. While an engine runs, that release is
refused with `ccol_not_permitted`.

## A per-request arena

An arena turns each allocation into a pointer increment and frees all the
memory of a batch of work in one step, so it's a good fit whenever a group
of objects dies together, like the work of one request. This loop works like
a simple server. It gives each "request" a map that lives in the arena,
destroys the map and then resets the arena:

```c
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ccollections/chashmap.h>

/* A bump arena: an allocation is a pointer increment, free does nothing,
 * and one step releases the whole arena. Each block has a header, exactly
 * one max_align_t wide, that holds its size (for realloc). */
typedef union { max_align_t align; size_t size; } arena_hdr;

static struct { char *base; size_t cap, used; } arena;

static void *arena_malloc(size_t n) {
    size_t unit = sizeof(arena_hdr);
    if (n > SIZE_MAX - 2 * unit) return NULL;
    size_t need = unit + (n + unit - 1) / unit * unit;
    if (need > arena.cap - arena.used) return NULL;
    arena_hdr *h = (arena_hdr *)(arena.base + arena.used);
    arena.used += need;
    h->size = n;
    return h + 1;
}
static void *arena_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *p = arena_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
static void *arena_realloc(void *old, size_t n) {
    void *p = arena_malloc(n);
    if (p && old) {
        size_t old_size = ((arena_hdr *)old - 1)->size;
        memcpy(p, old, old_size < n ? old_size : n);
    }
    return p;
}
static void arena_free(void *p) { (void)p; }   /* the arena releases all blocks in one step */

static ccol_memmgmt_procs_t arena_procs = {
    .malloc = arena_malloc, .free = arena_free,
    .calloc = arena_calloc, .realloc = arena_realloc,
};

/* Handles one "request": counts the words of a line in a map that lives in
 * the arena. The function destroys the map before it resets the arena. */
static void handle(const char *line) {
    chmap_construct_mp(seen, char *, int, &arena_procs);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", line);
    int distinct = 0;
    for (char *save = NULL, *w = strtok_r(buf, " ", &save); w;
         w = strtok_r(NULL, " ", &save)) {
        int *n = chmap_get_ptr(seen, w);
        if (n) (*n)++;
        else { chmap_insert(seen, w, 1); distinct++; }
    }
    printf("%-28s %d distinct words, arena used %zu bytes\n",
           line, distinct, arena.used);
    chmap_destroy(seen);
    arena.used = 0;                    /* this releases all blocks of this request */
}

int main(void) {
    arena.cap = 1 << 20;
    arena.base = malloc(arena.cap);    /* the block from malloc has the correct alignment */
    if (!arena.base) return 1;

    handle("a rose is a rose");
    handle("to be or not to be");
    handle("one two three four five");

    free(arena.base);                  /* after the destroy of all containers */
    return 0;
}
```

The header in front of each block is one `max_align_t` wide, so every
block is aligned for any object type, and without it the containers would
read memory that isn't correctly aligned. `free` does nothing because the
memory comes back when you reset the arena, so you must destroy every
container that uses the arena *before* you reset or release it.

The table has no context argument, so the arena has to be a global
variable. If many threads do this at the same time, give each thread its own
arena (with `_Thread_local`). And don't use an arena like this for a
`cmempool` pool that threads share, for the reason in the section above.

## Driving a container from a memory pool

`cmempool` can be the allocator of another container too. A ranged pool
has tiers whose sizes are powers of two and picks the right tier for each
request, so it fits an ordered map well, since that stores one small node
per entry:

```c
#include <stdint.h>
#include <stdio.h>
#include <ccollections/cbstmap.h>
#include <ccollections/cmempool.h>

/* An ordered map allocates one small node per entry, which is the pattern
 * that a pool suits best. This pool has tiers from 2^4 = 16 to 2^12 = 4096
 * bytes and 2^10 entries in its smallest tier, uses the heap only when
 * every tier is empty, and is thread-safe (single_threaded false). */
static ccol_r_mempool *node_pool;

static void *pool_malloc(size_t size) {
    return ccol_r_mempool_alloc_entry(node_pool, size);
}
static void *pool_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    return ccol_r_mempool_calloc_entry(node_pool, n * size);
}
static void *pool_realloc(void *p, size_t size) {
    return ccol_r_mempool_realloc_entry(node_pool, p, size);
}
static void pool_free(void *p) {
    ccol_r_mempool_free_entry(node_pool, p);
}

static ccol_memmgmt_procs_t pool_procs = {
    .malloc = pool_malloc, .free = pool_free,
    .calloc = pool_calloc, .realloc = pool_realloc,
};

int main(void) {
    char *err = NULL;
    node_pool = ccol_r_mempool_create(4, 12, 10,
                                      ccol_fallback_at_last_exhaustion,
                                      false, NULL, &err);
    if (!node_pool) { fprintf(stderr, "pool: %s\n", err); return 1; }

    cbmap_construct_mp(squares, int, long, &pool_procs);
    for (int i = 0; i < 5000; i++)
        cbmap_insert(squares, i, (long)i * i);
    printf("4999^2 = %ld\n", cbmap_get(squares, 4999));

    cbmap_destroy(squares);            /* all nodes go back to the pool */
    ccol_r_mempool_destroy(node_pool); /* destroy the pool last */
    return 0;
}
```

A ranged pool only gives out sizes up to its largest tier (4096 bytes
here), so a container whose arrays grow past that, like a large hash map or
a long vector, needs a bigger top tier or a different allocator. The
fallback policy decides what happens when a tier runs out of entries. With
`ccol_fallback_at_last_exhaustion` the pool only borrows from the heap when
all tiers are empty, and it frees such blocks just like any other entry.
Destroy the container first and the pool last.

The [cmempool guide](cmempool.md) explains how to choose tier sizes and
fallback policies.

## Pitfalls

A home-made allocator that returns blocks with only 8-byte alignment on a
64-bit machine causes undefined behavior, so round to
`_Alignof(max_align_t)`.

Don't release the allocator too early. Destroy every object that uses it
first, and keep in mind that shared `cmempool` pools can call it later, and
so can the engine of the HTTP client until `chttpcli_engine_wait()`
returns.

When a container gives you memory to free, a serialized string for example,
release it with the matching function of the module (such as
`cjson_serialize_free_mp`) so the memory goes back to the right allocator.

A `cmempool` pool can call its allocator while it holds its own lock, so
that allocator must never allocate from the same pool or free into it (for a
ranged pool that means any of its tiers). If it does, you get a deadlock or
a corrupted pool.

## Reference

[ccollections(7)](../man/common/ccollections.7) (the allocator contract),
[ccol_scoped_ptr_mp(3)](../man/common/ccol_scoped_ptr_mp.3),
[cvec_construct_mp(3)](../man/cvector/cvec_construct_mp.3),
[chmap_construct_mp(3)](../man/chashmap/chmap_construct_mp.3),
[cbmap_construct_mp(3)](../man/cbstmap/cbmap_construct_mp.3),
[cstr_construct_mp(3)](../man/cstring/cstr_construct_mp.3),
[cjson_parse_mp(3)](../man/cjson/cjson_parse_mp.3),
[cjson_serialize_free_mp(3)](../man/cjson/cjson_serialize_free_mp.3),
[ccol_circular_queue_create_mp(3)](../man/cthreadcomm/ccol_circular_queue_create_mp.3),
[clrucache_create_full(3)](../man/clrucache/clrucache_create_full.3),
[ctpool_create_mp(3)](../man/cthreadpool/ctpool_create_mp.3),
[chttpcli_set_engine_mem_mgmt_procs(3)](../man/chttpclient/chttpcli_set_engine_mem_mgmt_procs.3),
[chttpcli_engine_wait(3)](../man/chttpclient/chttpcli_engine_wait.3),
[ccol_mempool_create(3)](../man/cmempool/ccol_mempool_create.3),
[ccol_mempool_destroy(3)](../man/cmempool/ccol_mempool_destroy.3),
[ccol_r_mempool_create(3)](../man/cmempool/ccol_r_mempool_create.3),
[ccol_r_mempool_alloc_entry(3)](../man/cmempool/ccol_r_mempool_alloc_entry.3),
[ccol_r_mempool_free_entry(3)](../man/cmempool/ccol_r_mempool_free_entry.3)

Related guides:
[How the library works](design.md),
[Concurrency](concurrency.md),
[cmempool](cmempool.md)
