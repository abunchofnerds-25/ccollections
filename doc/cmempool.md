# Memory pools: `cmempool`

A memory pool allocates one block of memory at the start. It then gives out
parts of that block when the program asks for them. A pool gives you these
advantages over one `malloc` and one `free` for each object:

- O(1) allocation and free, with no system call for each request;
- no fragmentation inside the pool;
- a peak memory use that you know before the program runs, because a pool
  does not grow at run time.

`cmempool` gives two types of pool:

- a **fixed-size pool** (`ccol_mempool`), where all entries have the same size;
- a **ranged pool** (`ccol_r_mempool`), which keeps one fixed-size pool for
  each power-of-two size in a range. It serves each request from the
  smallest of these pools that is large enough.

Each type of pool can be thread-safe or single-threaded. Each type can use
the system allocator when it has no free entries. Each type can also be
fully inside a buffer that you supply, with no heap. Each entry that a pool
gives out has the alignment for all object types, as memory from `malloc()`
has.

**Use a pool when** your program creates and destroys many objects of one
size or of a small number of sizes. Network messages, game entities, parse
nodes and job records are examples. **Do not** use a pool when the sizes are
large and very different. Also do not use a pool when you cannot set a limit
on the number of objects that are live at the same time. For these cases, a
general-purpose allocator is the better tool.

```c
#include <ccollections/cmempool.h>
```

Link with `-lccollections -lpthread`.

## A first pool

```c
#include <ccollections/cmempool.h>
#include <stdio.h>

typedef struct {
    int    id;
    double score;
} record;

int main(void) {
    char *err = NULL;
    /* 256 records, no fallback to malloc, thread-safe, default allocator */
    ccol_mempool *pool = ccol_mempool_create(256, sizeof(record),
                                             false, false, NULL, &err);
    if (!pool) {
        fprintf(stderr, "pool: %s\n", err);
        return 1;
    }

    record *a = ccol_mempool_alloc_entry(pool);   /* not initialized */
    record *b = ccol_mempool_calloc_entry(pool);  /* all bytes are zero */
    a->id = 1;
    a->score = 9.5;
    b->id = 2;

    printf("in use: %zu of %zu\n", ccol_mempool_used_count(pool),
           ccol_mempool_total_capacity(pool));

    ccol_mempool_free_entry(pool, a);   /* a becomes NULL */
    ccol_mempool_free_entry(pool, b);
    ccol_mempool_destroy(pool);         /* pool becomes NULL */
    return 0;
}
```

The arguments of `ccol_mempool_create()` are, in this order:

1. the number of entries;
2. the size of each entry;
3. whether the pool uses the heap when it has no free entries;
4. whether only one thread uses the pool;
5. an optional custom allocator (`NULL` for `malloc`/`free`);
6. an optional pointer that gets an error message.

The message is a static string. Do not free it.

`ccol_mempool_free_entry()` and `ccol_mempool_destroy()` are macros. They also
set your pointer to `NULL`. Therefore, you cannot use an old pointer by
accident.

## The rules of a fixed-size pool

- **Free an entry to the pool that gave it.** The pool examines each address
  that you give back before it reads through that address. These errors stop
  the program with an assertion: an entry of a different pool, an address
  that the pool did not give out, and a double free. They do not corrupt memory.
- **You can free `NULL`**, as with `free()`.
- **Entries are not initialized**, unless you use
  `ccol_mempool_calloc_entry()`.
- **The pool increases an entry size that is less than `sizeof(uintptr_t)`** to
  that value. Therefore, very small entries also work. The pool refuses a size
  of 0 or a count of 0.

## When the pool has no free entries

When you create a pool, you select what it does when it has no free entries:

- **No fallback** (`false`): the next allocation gives `NULL`. Use this
  setting when "full" is a correct answer, for example "too many connections"
  or "too many bullets on the screen". Also use it when the program must not
  use the heap after it starts.
- **Fallback** (`true`): the pool serves the request from the heap through its
  allocator, and the call is successful. Free that entry to the pool in the
  same way as all other entries.

```c
#include <ccollections/cmempool.h>
#include <stdio.h>

int main(void) {
    ccol_mempool *strict = ccol_mempool_create(4, 32, false, true, NULL, NULL);
    ccol_mempool *elastic = ccol_mempool_create(4, 32, true, true, NULL, NULL);
    if (!strict || !elastic) {
        ccol_mempool_destroy(strict);               /* NULL is permitted */
        ccol_mempool_destroy(elastic);
        return 1;
    }
    void *a[5], *b[5];

    for (int i = 0; i < 5; i++) {
        a[i] = ccol_mempool_alloc_entry(strict);    /* 5th call: NULL */
        b[i] = ccol_mempool_alloc_entry(elastic);   /* 5th call: from malloc */
    }
    printf("strict 5th: %s\n", a[4] ? "entry" : "NULL");
    printf("elastic fallback entries: %zu\n",
           ccol_mempool_dynamic_allocs_count(elastic));

    for (int i = 0; i < 5; i++) {
        ccol_mempool_free_entry(strict, a[i]);      /* NULL is permitted */
        ccol_mempool_free_entry(elastic, b[i]);     /* also fallback entries */
    }
    ccol_mempool_destroy(strict);
    ccol_mempool_destroy(elastic);
    return 0;
}
```

`ccol_mempool_dynamic_allocs_count()` gives the number of fallback entries
that are not free. A value that is not zero shows that the pool was too small
for its work at some time. When you see this signal, increase the number of
entries. Free all fallback entries before you destroy the pool. The destroy
operation stops the program when a fallback entry is not free, because it
is a leak.

## Example: bullets in a game loop

An arcade game creates many bullets in each frame, and each bullet has a
short life. A pool with no fallback makes "the screen is full" a plain
`NULL`. The pool keeps all bullets in one block of memory. The peak memory
is fixed when you create the pool, and the number of bullets on the screen
does not change it.

```c
#include <ccollections/cmempool.h>
#include <stdio.h>

#define MAX_BULLETS 64

typedef struct {
    float x, y;
    float vx, vy;
    int   frames_left;
} bullet;

static ccol_mempool *bullet_pool;
static bullet       *live[MAX_BULLETS];
static size_t        live_count;

static bool fire(float x, float y, float vx, float vy) {
    bullet *b = ccol_mempool_calloc_entry(bullet_pool);
    if (!b)
        return false;               /* the screen is full: ignore the shot */
    *b = (bullet){ x, y, vx, vy, 3 };
    live[live_count++] = b;
    return true;
}

static void step(void) {
    for (size_t i = 0; i < live_count;) {
        bullet *b = live[i];
        b->x += b->vx;
        b->y += b->vy;
        if (--b->frames_left == 0) {
            live[i] = live[--live_count];          /* remove with a swap */
            ccol_mempool_free_entry(bullet_pool, b);
        } else {
            i++;
        }
    }
}

int main(void) {
    /* One game thread, therefore no lock. No fallback: a full pool means
       "too many bullets", and fire() reports it. */
    bullet_pool = ccol_mempool_create(MAX_BULLETS, sizeof(bullet),
                                      false, true, NULL, NULL);
    if (!bullet_pool)
        return 1;

    int dropped = 0;
    for (int frame = 0; frame < 10; frame++) {
        for (int shot = 0; shot < 30; shot++)
            if (!fire(0.0f, 0.0f, 1.0f, (float)shot))
                dropped++;
        step();
    }
    printf("live %zu, dropped %d, pool in use %zu\n", live_count, dropped,
           ccol_mempool_used_count(bullet_pool));

    while (live_count > 0)
        ccol_mempool_free_entry(bullet_pool, live[--live_count]);
    ccol_mempool_destroy(bullet_pool);
    return 0;
}
```

The example creates the pool with `single_threaded = true`, because only the
game loop uses it. That pool does not use a lock. Do not share such a pool
between threads.

## Sharing a pool between threads

Create the pool with `single_threaded = false`. Then many threads can
allocate and free at the same time. On the usual path, a thread-safe pool
does not use a lock. Each thread keeps a small private cache of entries. The
thread uses the shared free list only to fill its cache or to empty it.
Therefore, the throughput increases when you add threads, and the threads do not
all wait on one lock.

This cache causes some sizing rules. Know them before you select the number
of entries:

- **Give a thread-safe pool a minimum of 8 entries.** A smaller pool has no
  space for caches, and each call uses the lock. 8 is a minimum, not a
  target. A larger pool gives each thread a larger cache.
- **A pool gives out a maximum of one cache for each 8 entries, and a maximum
  of 16 caches.** A thread that finds no free cache also works, with one lock
  for each call. It gets a cache when a thread with a cache stops. When many
  threads share a pool, make the pool large enough to give each thread a
  cache.
- **A pool with caches keeps a reserve above the count that you set.**
  Therefore, each thread can get that count, whatever the other threads keep in
  their caches. `ccol_mempool_total_capacity()` gives your count. To size
  against a memory budget, use `ccol_mempool_allocated_bytes()`. It gives the
  real cost of the pool, with the reserve. The reserve is a large part of a
  small pool and a few percent of a large pool.
- **Join the threads that used a pool before you destroy it.** The cache of
  a thread goes back to the pool when the thread stops.

Single-threaded pools and pools on your own buffer have no caches. Therefore,
these rules do not apply to them. For the details, and for the accuracy of the
counts when many threads use the pool, see
[ccol_mempool_create(3)](../man/cmempool/ccol_mempool_create.3) and
[ccol_mempool_total_capacity(3)](../man/cmempool/ccol_mempool_total_capacity.3).

### Example: worker threads allocating job records

```c
#include <ccollections/cmempool.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define WORKERS      4
#define JOBS_EACH    100000
#define POOL_ENTRIES 1024          /* large enough: each of 4 workers gets a cache */

typedef struct {
    unsigned id;
    char     payload[48];
} job;

static ccol_mempool *job_pool;

static void *worker(void *arg) {
    unsigned base = (unsigned)(size_t)arg * JOBS_EACH;
    unsigned long checksum = 0;

    for (unsigned i = 0; i < JOBS_EACH; i++) {
        job *j = ccol_mempool_alloc_entry(job_pool);
        if (!j)
            continue;                       /* not possible with a fallback */
        j->id = base + i;
        snprintf(j->payload, sizeof(j->payload), "job-%u", j->id);
        checksum += strlen(j->payload);
        ccol_mempool_free_entry(job_pool, j);
    }
    return (void *)checksum;
}

int main(void) {
    char *err = NULL;
    /* Thread-safe. It uses the heap when the pool has no free entries. */
    job_pool = ccol_mempool_create(POOL_ENTRIES, sizeof(job), true, false,
                                   NULL, &err);
    if (!job_pool) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    pthread_t tids[WORKERS];
    size_t started = 0;
    while (started < WORKERS &&
           pthread_create(&tids[started], NULL, worker, (void *)started) == 0)
        started++;

    unsigned long total = 0;
    for (size_t t = 0; t < started; t++) {      /* join BEFORE destroy */
        void *sum;
        pthread_join(tids[t], &sum);
        total += (unsigned long)sum;
    }

    if (started < WORKERS)
        fprintf(stderr, "only %zu of %d workers started\n", started, WORKERS);
    printf("payload bytes: %lu\n", total);
    printf("capacity %zu, block %zu bytes, fallback entries %zu\n",
           ccol_mempool_total_capacity(job_pool),
           ccol_mempool_allocated_bytes(job_pool),
           ccol_mempool_dynamic_allocs_count(job_pool));

    ccol_mempool_destroy(job_pool);
    return 0;
}
```

The last line of the output shows two values. The first is the count that
you set (1024 entries). The second is the memory that the pool keeps for
them, with the reserve. The two values are fixed when you create the pool.

## Ranged pools: many sizes, one allocator

A ranged pool covers a range of power-of-two sizes. You give it three
exponents: the smallest size, the largest size, and the entry count of the
smallest tier. Each larger tier has half the entries of the tier below it.
`ccol_r_mempool_create(4, 12, 9, ...)` makes these tiers:

| Entry size | Entries |
|---|---|
| 2^4 = 16 bytes | 2^9 = 512 |
| 2^5 = 32 bytes | 256 |
| 2^6 = 64 bytes | 128 |
| ... | ... |
| 2^11 = 2048 bytes | 4 |
| 2^12 = 4096 bytes | 2 |

The smallest tier that is large enough serves a request. Therefore, a request
for 100 bytes gets a 128-byte entry. A request that is larger than the largest
tier gives `NULL`. A request of 0 bytes also gives `NULL`. The smallest tier
size is 16 bytes (exponent 4).

The fallback policy controls what occurs when a tier has no free entries:

- `ccol_fallback_disabled`: try the larger tiers, then return `NULL`.
- `ccol_fallback_at_first_exhaustion`: the tier that fits the request serves
  it from the heap when that tier has no free entries.
- `ccol_fallback_at_last_exhaustion`: try the larger tiers. Use the heap only
  when all tiers that can serve the request have no free entries.

`ccol_r_mempool_realloc_entry()` changes the size of an entry. When the entry
has enough bytes, the function gives the same pointer and does not copy.
In other cases, it moves the data to a tier that fits. A failed call gives
`NULL`, as with `realloc()`. Your initial entry stays valid, and it stays
yours.

The free macro takes the ranged pool, not the tier. The pool finds the tier
from the address.

### Example: a growable string in a ranged pool

```c
#include <ccollections/cmempool.h>
#include <stdio.h>
#include <string.h>

/* A small string that can grow. A ranged pool holds its storage. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} pstr;

static ccol_r_mempool *strings;

static bool pstr_append(pstr *s, const char *text) {
    size_t need = s->len + strlen(text) + 1;
    if (need > s->cap) {
        size_t cap = s->cap ? s->cap : 16;
        while (cap < need)
            cap *= 2;
        char *grown = ccol_r_mempool_realloc_entry(strings, s->data, cap);
        if (!grown)
            return false;          /* s->data stays valid and stays ours */
        s->data = grown;
        s->cap = cap;
    }
    memcpy(s->data + s->len, text, strlen(text) + 1);
    s->len = need - 1;
    return true;
}

int main(void) {
    /* Tiers of 16, 32, ..., 1024 bytes; 2^8 = 256 entries of 16 bytes,
       and half the entries in each larger tier. Use the heap only when
       all tiers have no free entries. */
    strings = ccol_r_mempool_create(4, 10, 8,
                                    ccol_fallback_at_last_exhaustion,
                                    true, NULL, NULL);
    if (!strings)
        return 1;

    const char *words[] = { "pools", "hand", "out", "power-of-two", "slices",
                            "of", "memory", "that", "they", "allocated",
                            "up", "front" };
    pstr sentence = { 0 };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (!pstr_append(&sentence, i ? " " : "") ||
            !pstr_append(&sentence, words[i])) {
            fprintf(stderr, "out of memory\n");
            ccol_r_mempool_free_entry(strings, sentence.data);  /* NULL is permitted */
            ccol_r_mempool_destroy(strings);
            return 1;
        }
    }
    printf("%s (%zu bytes, entry of %zu)\n", sentence.data, sentence.len,
           sentence.cap);
    printf("64-byte tier: %zu of %zu in use\n",
           ccol_r_mempool_used_count(strings, 64),
           ccol_r_mempool_total_capacity(strings, 64));

    ccol_r_mempool_free_entry(strings, sentence.data);
    ccol_r_mempool_destroy(strings);
    return 0;
}
```

The queries for one size (`ccol_r_mempool_used_count()`,
`ccol_r_mempool_total_capacity()`, `ccol_r_mempool_dynamic_allocs_count()`)
take a size. They give the values of the tier that serves that size.

Each tier is a separate pool. Therefore, the sizing rules of a thread-safe pool
apply to **each tier**. The largest tiers have the fewest entries, and they
go below 8 entries first. To keep them at 8 or more, increase the count
exponent or make the range smaller. See
[ccol_r_mempool_create(3)](../man/cmempool/ccol_r_mempool_create.3).

## Pools without a heap: preallocated buffers

Some embedded code and real-time code must not get entry storage from the
heap. For this code, make the pool on a buffer that you declare. The two
`CCOL_DECLARE_PREALLOCATED_*` macros declare an array with the correct size
and alignment for the pool parameters that you give. Therefore, you do not
calculate the size yourself.

```c
#include <ccollections/cmempool.h>
#include <stdio.h>

typedef struct {
    unsigned char bytes[40];
} frame;

/* Storage for 32 frames. The library sets the size and the alignment. */
CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER(frame_storage, 32, sizeof(frame));

/* Storage for a ranged pool: tiers of 16..256 bytes, 2^6 = 64 entries of
   16 bytes, and half the entries in each larger tier. */
CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(scratch_storage, 4, 8, 6);

int main(void) {
    char *err = NULL;

    ccol_mempool *frames = ccol_mempool_create_from_preallocated_buffer(
        frame_storage, sizeof(frame_storage), sizeof(frame),
        false, true, NULL, &err);
    if (!frames) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    ccol_r_mempool *scratch = ccol_r_mempool_create_from_preallocated_buffer(
        scratch_storage, sizeof(scratch_storage), 4, 8, 6,
        ccol_fallback_disabled, true, NULL, &err);
    if (!scratch) {
        fprintf(stderr, "%s\n", err);
        ccol_mempool_destroy(frames);
        return 1;
    }

    frame *f = ccol_mempool_alloc_entry(frames);
    char  *s = ccol_r_mempool_alloc_entry(scratch, 100);  /* 128-byte tier */
    printf("frames: %zu slots, scratch 128-byte tier: %zu slots\n",
           ccol_mempool_total_capacity(frames),
           ccol_r_mempool_total_capacity(scratch, 100));

    ccol_r_mempool_free_entry(scratch, s);
    ccol_mempool_free_entry(frames, f);
    ccol_r_mempool_destroy(scratch);   /* the buffers stay; they are ours */
    ccol_mempool_destroy(frames);
    return 0;
}
```

Important facts:

- The pool handle comes from the allocator (or from `malloc`). Only the
  entries are in your buffer.
- The destroy operation does not free your buffer.
- A buffer that you declare manually must have an alignment of a minimum of
  16 bytes. If not, the create operation fails. The macros give this
  alignment.
- The ranged macro refuses parameters that are not possible, at compile time.
- A pool on a preallocated buffer keeps no caches for threads. Therefore, its
  counts are exact, and it has no reserve.

## Using a pool as the allocator of other containers

Each container in this library accepts a `ccol_memmgmt_procs_t *`. A pool
(usually a ranged pool) can supply the functions behind it.
[Memory management](memory.md) shows the full pattern.

The allocator that you give *to* a pool must obey one rule. The pool calls it
for fallback entries while the pool changes its own state. If the pool has a
lock, the pool holds the lock during this call. Therefore, that allocator must
not call back into the same pool. For a ranged pool, it must not call any tier
of the pool. An allocator that gets memory from a *different* pool is always
safe.

## Choosing the numbers

The correct entry count, entry size, threading mode and tier range depend on
your program. They depend on the sizes of its objects, on its number of
threads, and on the life of its objects. No default value is correct for all
programs. Measure your own configuration. Do not copy a different one. In the
source tree, `make bench` runs the pool benchmarks next to the system
allocator on your machine. `make bench BENCH_ARGS="--filter=cmempool"` runs
only the pool benchmarks. Compare the ratios against `malloc`, not the
absolute times.

In production, `ccol_mempool_dynamic_allocs_count()` shows whether a pool with
a fallback is too small.

## Pitfalls

- **Free each entry to the pool that gave it.** When you free an entry to a
  different pool, the program stops. Keep the pool near the objects that it
  serves.
- **Free all fallback entries before you destroy the pool.** A destroy while
  fallback entries are in use stops the program.
- **Use a `single_threaded = true` pool from one thread only.** That pool
  has no lock, and two threads cause a data race.
- **Join the threads that use a shared pool before you destroy it.**
- **Different build settings.** The preallocated-buffer macros size your
  array with the same layout setting as the library
  (`CCOL_MEMPOOL_COMPACT_LAYOUT`). If the settings are different, the link
  fails. See [Building](building.md).

## Reference

Fixed-size pool:

- [ccol_mempool_create(3)](../man/cmempool/ccol_mempool_create.3)
- [ccol_mempool_create_from_preallocated_buffer(3)](../man/cmempool/ccol_mempool_create_from_preallocated_buffer.3)
- [CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER(3)](../man/cmempool/CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER.3)
- [ccol_mempool_alloc_entry(3)](../man/cmempool/ccol_mempool_alloc_entry.3),
  [ccol_mempool_calloc_entry(3)](../man/cmempool/ccol_mempool_calloc_entry.3),
  [ccol_mempool_free_entry(3)](../man/cmempool/ccol_mempool_free_entry.3)
- [ccol_mempool_destroy(3)](../man/cmempool/ccol_mempool_destroy.3)
- [ccol_mempool_total_capacity(3)](../man/cmempool/ccol_mempool_total_capacity.3),
  [ccol_mempool_used_count(3)](../man/cmempool/ccol_mempool_used_count.3),
  [ccol_mempool_dynamic_allocs_count(3)](../man/cmempool/ccol_mempool_dynamic_allocs_count.3),
  [ccol_mempool_allocated_bytes(3)](../man/cmempool/ccol_mempool_allocated_bytes.3)

Ranged pool:

- [ccol_r_mempool_create(3)](../man/cmempool/ccol_r_mempool_create.3)
- [ccol_r_mempool_create_from_preallocated_buffer(3)](../man/cmempool/ccol_r_mempool_create_from_preallocated_buffer.3)
- [CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(3)](../man/cmempool/CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER.3)
- [ccol_r_mempool_alloc_entry(3)](../man/cmempool/ccol_r_mempool_alloc_entry.3),
  [ccol_r_mempool_calloc_entry(3)](../man/cmempool/ccol_r_mempool_calloc_entry.3),
  [ccol_r_mempool_realloc_entry(3)](../man/cmempool/ccol_r_mempool_realloc_entry.3),
  [ccol_r_mempool_free_entry(3)](../man/cmempool/ccol_r_mempool_free_entry.3)
- [ccol_r_mempool_destroy(3)](../man/cmempool/ccol_r_mempool_destroy.3)
- [ccol_r_mempool_total_capacity(3)](../man/cmempool/ccol_r_mempool_total_capacity.3),
  [ccol_r_mempool_used_count(3)](../man/cmempool/ccol_r_mempool_used_count.3),
  [ccol_r_mempool_dynamic_allocs_count(3)](../man/cmempool/ccol_r_mempool_dynamic_allocs_count.3),
  [ccol_r_mempool_allocated_bytes(3)](../man/cmempool/ccol_r_mempool_allocated_bytes.3)

Related guides:

- [Memory management](memory.md): custom allocators, and containers that use
  a ranged pool
- [Concurrency](concurrency.md): thread safety and the `fork()` policy
- [Building](building.md): compile-time configuration, for example
  `CCOL_MEMPOOL_COMPACT_LAYOUT`
