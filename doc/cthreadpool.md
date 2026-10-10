# cthreadpool: running tasks on a pool of threads

Starting a new thread for each small piece of work is expensive, because
every `pthread_create` allocates a stack and makes system calls. A thread pool
instead starts a fixed number of worker threads once and keeps them: you
submit a task (a function and an argument), and the next free worker runs it.
The pool limits how many threads run at the same time and removes the cost of
creating a thread for each task.

`cthreadpool` gives you:

- a bounded or unbounded task queue,
- blocking, non-blocking and timed submits,
- an optional completion callback for each task,
- **futures**, which let the thread that submits a task get the `void *`
  result of that task,
- a barrier (`ctpool_wait`) and two shutdown modes.

Use it when you have independent pieces of work that can run in parallel,
such as parsing files, hashing blocks or answering requests. Use
[cthreadcomm](cthreadcomm.md) instead when you manage your own long-lived
threads and they only need to exchange messages.

```c
#include <ccollections/cthreadpool.h>
```

## A first example

```c
#include <ccollections/cthreadpool.h>
#include <stdio.h>

typedef struct { int input; long output; } item_t;

static void square(void *arg) {
    item_t *it = arg;
    it->output = (long)it->input * it->input;
}

int main(void) {
    char *err = NULL;
    /* 4 worker threads, a bounded queue of 64 tasks */
    ctpool pool = ccol_create_cthread_pool(4, 64, &err);
    if (!pool) { fprintf(stderr, "pool: %s\n", err); return 1; }

    item_t items[8];
    for (int i = 0; i < 8; i++) {
        items[i].input = i + 1;
        if (ctpool_submit(pool, square, &items[i], NULL) != ccol_success)
            items[i].output = -1;
    }

    ctpool_wait(pool);                 /* all submitted tasks are complete */
    for (int i = 0; i < 8; i++)
        printf("%d^2 = %ld\n", items[i].input, items[i].output);

    ctpool_destroy(pool);              /* empties, joins, sets pool to CTPOOL_INVALID */
    return 0;
}
```

Build it with `-lccollections -lpthread`. The pool starts all its workers
before `ccol_create_cthread_pool` returns, so it is ready as soon as you have
the handle.

## Creating a pool

```c
char *err = NULL;
ctpool pool = ccol_create_cthread_pool(num_threads, queue_capacity, &err);
if (!pool) { /* err gives the cause */ }
```

`queue_capacity` selects the queue mode once, for the life of the pool:

- **`0`**: an unbounded queue. A submit never waits for space and fails only
  when no memory is available.
- **`N > 0`**: a bounded queue of `N` tasks. A plain submit waits while the
  queue is full, so a producer can go only as fast as the workers.

`ctpool` is an opaque value handle (an integer), not a pointer. Compare it
with `CTPOOL_INVALID`, which is `0`, so `if (!pool)` works. The library checks
every use of the handle, so it detects a handle whose pool you destroyed
instead of dereferencing it.

The macro forms declare the variable for you and stop the program through
`ccol_fatal_err` if creation fails, which is useful when no correct recovery is possible:

```c
/* fragment */
ctpool_construct(pool, 4, 256);          /* you must destroy it later */
ctpool_construct_scoped(pool2, 4, 0);    /* destroyed at the end of the scope */
```

`ccol_create_cthread_pool_mp` takes a custom allocator for the pool's own
memory (see [Memory management](memory.md)). Futures always use the standard
`malloc` and `free`.

## Submitting tasks

A task is `void fn(void *arg)`. Three submit functions cover the usual kinds
of producer:

```c
/* fragment */
ctpool_submit(pool, fn, arg, on_complete);              /* waits for space */
ctpool_try_submit(pool, fn, arg, on_complete);          /* ccol_container_full immediately */
ctpool_timed_submit(pool, fn, arg, on_complete, 50000); /* waits at most 50 ms */
```

The timeout is a `uint64_t` count of microseconds, measured on a monotonic
clock. With a timeout of `0`, the timed submit behaves like a `try` submit.
[The design guide](design.md) describes the timeout conventions of the
library.

The pool never frees `arg`; it belongs to you, and `on_complete` is the place
to release it.

### The completion callback

`on_complete` has the prototype `void on_complete(void *arg, bool ran)` and
can be `NULL`. The pool calls it **exactly one time for each task that a
submit accepted**:

- `on_complete(arg, true)` on the worker, immediately after `fn` returns;
- `on_complete(arg, false)` when `ctpool_shutdown_immediate` discards the
  task before a worker took it, in which case `fn` does not run.

A task that owns a heap argument can therefore free it in `on_complete`,
whatever happens to the task. A failed submit accepted nothing and does not
call `on_complete`, so after a failed submit you must free the argument
yourself.

## Futures: getting a result back

A future task is `void *fn(void *arg)`, and its return value goes back to
every thread that waits on the future:

```c
/* fragment */
ctpool_future *f = ctpool_submit_future(pool, compute, arg);
if (f) {
    void *result = ctpool_future_get(f);   /* waits until the task is complete */
    /* ... use result, then free it the way compute allocated it ... */
    ctpool_future_free(f);                 /* release your handle exactly once */
}
```

Three rules keep futures correct:

- **The result is yours.** The pool only passes the pointer along; it neither
  copies nor frees the result. `ctpool_future_free` frees the future, not the
  result.
- **Call `ctpool_future_free` exactly one time for each future**, even when
  you never called `ctpool_future_get`. A free without a get means "run the
  task and ignore it": the task runs, and the pool discards its result.
- **Many threads can wait** in `ctpool_future_get` on one future, and they all
  receive the same result. Because there is only one future, it gets one
  `ctpool_future_free` in total.

`ctpool_future_done` checks the future without waiting.
`ctpool_try_submit_future` and `ctpool_timed_submit_future` are the
non-blocking and timed submits; they return a status code and hand back the
future through a pointer, so you can tell a full queue, a shutdown and a lack
of memory apart.

## Waiting and shutting down

`ctpool_wait` is a barrier: it returns when the queue is empty and no task is
running. The pool keeps existing afterwards, so you can submit the next phase:

```c
/* fragment */
for (size_t i = 0; i < n; i++) ctpool_submit(pool, phase1, &items[i], NULL);
ctpool_wait(pool);       /* all of phase 1 is complete */
for (size_t i = 0; i < n; i++) ctpool_submit(pool, phase2, &items[i], NULL);
```

Do not let another thread submit while you wait: with an unbounded queue, the
wait might never end.

There are two ways to stop a pool:

- **`ctpool_shutdown_drain`** finishes every queued and running task, then
  stops the workers.
- **`ctpool_shutdown_immediate`** discards the queued tasks, calling
  `on_complete(arg, false)` for each one, and marks each discarded future as
  cancelled (`ctpool_future_get` returns `NULL` and `ctpool_future_cancelled`
  is true). The running tasks finish, and then the function stops the
  workers.

Both functions wait, for every caller, until all workers have stopped. After
a shutdown, a submit returns `ccol_not_permitted` and `ctpool_submit_future`
returns `NULL`. `ctpool_destroy` frees the pool and sets the handle to
`CTPOOL_INVALID`; if you did not call a shutdown function, it finishes the
queued tasks first.

## Detached futures

Sometimes the result comes from a thread that is not a pool worker, such as a
network thread, an event loop callback or a hardware completion. A **detached
future** is the same `ctpool_future`, created without a pool: the producer
calls `ctpool_future_fulfill` one time, and the consumer uses the usual
`ctpool_future_get` and `ctpool_future_free`. A full example follows below.

## Example: counting words chapter by chapter

Each chapter is an independent task, a shared atomic counter collects the
total for the book, and `ctpool_wait` is the barrier before the report.
Because the pool is scoped, the program destroys it at the end of its block.

```c
/* Count the words of a book, one chapter for each task, then report. */
#include <ccollections/cthreadpool.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>

typedef struct {
    const char *title;
    const char *text;
    size_t words;                 /* only one task writes it */
    atomic_size_t *book_total;    /* all tasks share it */
} chapter_t;

static void count_chapter(void *arg) {
    chapter_t *ch = arg;
    size_t n = 0;
    int in_word = 0;
    for (const char *p = ch->text; *p; p++) {
        if (isspace((unsigned char)*p)) in_word = 0;
        else if (!in_word) { in_word = 1; n++; }
    }
    ch->words = n;
    atomic_fetch_add(ch->book_total, n);
}

int main(void) {
    atomic_size_t total = 0;
    chapter_t book[] = {
        { "Arrival",  "It was a dark and stormy night. The train was late.", 0, &total },
        { "The Inn",  "Nobody at the inn had heard of the professor.",      0, &total },
        { "Clues",    "A torn ticket, a wet umbrella, and a missing key.",  0, &total },
        { "Solution", "The butler, of course. He always had the key.",      0, &total },
    };
    const size_t n = sizeof(book) / sizeof(book[0]);

    {
        /* Scoped: destroyed at the end of this block, after every task is
           complete. 0 means an unbounded queue, so the loop does not wait. */
        ctpool_construct_scoped(pool, 3, 0);
        for (size_t i = 0; i < n; i++)
            if (ctpool_submit(pool, count_chapter, &book[i], NULL) != ccol_success)
                fprintf(stderr, "could not submit %s\n", book[i].title);
        ctpool_wait(pool);        /* barrier: the count of each chapter is done */
    }

    for (size_t i = 0; i < n; i++)
        printf("%-9s %2zu words\n", book[i].title, book[i].words);
    printf("book      %2zu words\n", atomic_load(&total));
    return 0;
}
```

## Example: parallel search with futures

The example splits the range into eight tasks, each of which returns a
heap-allocated count. The main thread collects the futures in order, so the
output is the same however the tasks are scheduled.

```c
/* Divide a search into ranges, run each range as a task with a future,
   and collect the answers in order. */
#include <ccollections/cthreadpool.h>
#include <stdio.h>
#include <stdlib.h>

#define RANGES 8
#define LIMIT  200000UL

typedef struct { unsigned long lo, hi; } range_t;

static int is_prime(unsigned long n) {
    if (n < 2) return 0;
    for (unsigned long d = 2; d * d <= n; d++)
        if (n % d == 0) return 0;
    return 1;
}

/* A future task returns void *. The pool does not free that pointer:
   the caller of ctpool_future_get owns it. */
static void *count_primes(void *arg) {
    const range_t *r = arg;
    unsigned long *count = malloc(sizeof(*count));
    if (!count) return NULL;
    *count = 0;
    for (unsigned long n = r->lo; n < r->hi; n++)
        *count += (unsigned long)is_prime(n);
    return count;
}

int main(void) {
    ctpool pool = ccol_create_cthread_pool(4, 0, NULL);
    if (!pool) return 1;

    range_t ranges[RANGES];
    ctpool_future *futures[RANGES];
    for (int i = 0; i < RANGES; i++) {
        ranges[i].lo = LIMIT / RANGES * (unsigned long)i;
        ranges[i].hi = LIMIT / RANGES * (unsigned long)(i + 1);
        futures[i] = ctpool_submit_future(pool, count_primes, &ranges[i]);
    }

    unsigned long total = 0;
    for (int i = 0; i < RANGES; i++) {
        if (!futures[i]) continue;                 /* the submit failed */
        unsigned long *count = ctpool_future_get(futures[i]);  /* waits */
        if (count) {
            printf("[%6lu, %6lu): %4lu primes\n", ranges[i].lo, ranges[i].hi, *count);
            total += *count;
            free(count);                           /* we own the result */
        }
        ctpool_future_free(futures[i]);            /* exactly once for each future */
    }
    printf("primes below %lu: %lu\n", LIMIT, total);

    ctpool_destroy(pool);
    return 0;
}
```

## Example: a bounded job queue with an emergency stop

The pool has two workers and a queue of four slots, and the producer waits at
most 50 ms for space for each job. Each job owns a heap argument, which
`on_complete` frees both when the job ran and when the emergency stop
discarded it, so nothing leaks in either case.

```c
/* A bounded job queue: the producer feels backpressure, each job owns
   its own heap argument, and on_complete frees it whether the job ran
   or not. An emergency stop discards the jobs that are in the queue. */
#include <ccollections/cthreadpool.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct { int id; } job_t;

static atomic_int ran, skipped;

static void run_job(void *arg) {
    job_t *job = arg;
    (void)job;
    struct timespec ts = { 0, 20 * 1000000L };   /* simulate 20 ms of work */
    nanosleep(&ts, NULL);
}

/* Called exactly once for each accepted job: ran == true after run_job,
   ran == false when ctpool_shutdown_immediate discarded it before it ran. */
static void job_done(void *arg, bool ran_it) {
    atomic_fetch_add(ran_it ? &ran : &skipped, 1);
    free(arg);
}

int main(void) {
    ctpool pool = ccol_create_cthread_pool(2, 4, NULL);   /* 2 workers, 4 slots */
    if (!pool) return 1;

    int accepted = 0, refused = 0;
    for (int i = 0; i < 20; i++) {
        job_t *job = malloc(sizeof(*job));
        if (!job) {
            refused++;            /* no memory: this job is not submitted */
            continue;
        }
        job->id = i;
        /* Wait at most 50 ms for a free slot; if none, skip this job. */
        ccol_retval_t rc = ctpool_timed_submit(pool, run_job, job, job_done, 50000);
        if (rc == ccol_success) {
            accepted++;
        } else {
            refused++;            /* the pool did not accept the job: free it */
            free(job);
        }
    }

    /* Emergency stop: running jobs finish, and the pool discards the queued
       jobs, calling job_done(arg, false) for each before this call returns. */
    ctpool_shutdown_immediate(pool);
    ctpool_destroy(pool);

    printf("accepted %d, refused %d\n", accepted, refused);
    printf("ran + skipped = %d (matches accepted: %s)\n",
           atomic_load(&ran) + atomic_load(&skipped),
           atomic_load(&ran) + atomic_load(&skipped) == accepted ? "yes" : "no");
    return 0;
}
```

## Example: a result from outside the pool

```c
/* A detached future: a thread that is not a pool worker (here a simulated
   "network" thread) gives one result back to a caller that waits. */
#include <ccollections/cthreadpool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *network_thread(void *arg) {
    ctpool_future *reply = arg;
    struct timespec ts = { 0, 30 * 1000000L };   /* the "response" takes 30 ms */
    nanosleep(&ts, NULL);
    ctpool_future_fulfill(reply, strdup("HTTP/1.1 200 OK"));  /* exactly once */
    return NULL;
}

int main(void) {
    char *err = NULL;
    ctpool_future *reply = ctpool_future_create_detached(&err);
    if (!reply) { fprintf(stderr, "future: %s\n", err); return 1; }

    pthread_t t;
    if (pthread_create(&t, NULL, network_thread, reply) != 0) {
        ctpool_future_fulfill(reply, NULL);      /* the producer's reference */
        ctpool_future_free(reply);               /* our reference */
        return 1;
    }

    printf("done before the wait: %s\n", ctpool_future_done(reply) ? "yes" : "no");
    char *status = ctpool_future_get(reply);     /* waits until fulfilled */
    printf("status line: %s\n", status ? status : "(no memory for the reply)");
    free(status);                                /* the result is ours */
    ctpool_future_free(reply);                   /* our reference */

    pthread_join(t, NULL);
    return 0;
}
```

`ctpool_future_fulfill` can run before or after the consumer starts waiting,
and the consumer may even free its reference first: the library releases the
future when the last holder releases its reference.

## Good to know

- **Do not destroy a pool from one of its own tasks**, or from the
  `on_complete` of one of its tasks; that is a fatal error. `ctpool_wait` and
  the two shutdown functions do nothing when called from these places,
  because a worker cannot wait for itself.
- **A task that submits to its own bounded pool** gets `ccol_container_full`
  when the queue is full (or `NULL` from `ctpool_submit_future`) instead of
  waiting, because only the workers can make space. In that situation, use
  `ctpool_try_submit`, `ctpool_timed_submit` or an unbounded queue.
- **Do not destroy a pool through an old copy of its handle.**
  `ctpool_destroy` sets your variable to `CTPOOL_INVALID`, so a second destroy
  of that variable does nothing, but a destroy through a different copy of the
  same handle stops the program (it does not corrupt memory).
- **`ctpool_wait` does not wait for the callbacks of discarded tasks.** When
  another thread calls `ctpool_shutdown_immediate`, the return of that call is
  what tells you that all the `on_complete(arg, false)` calls have finished.
- **A custom allocator does not see one allocation per task.** The pool
  reuses its task records, so a counting allocator sees calls only
  occasionally.
- **`fork(2)`**: create pools after you fork, or fork and call `exec`
  immediately. See [Concurrency](concurrency.md).

## Reference

Creating and destroying:
[ccol_create_cthread_pool(3)](../man/cthreadpool/ccol_create_cthread_pool.3),
[ccol_create_cthread_pool_mp(3)](../man/cthreadpool/ccol_create_cthread_pool_mp.3),
[ctpool_declare(3)](../man/cthreadpool/ctpool_declare.3),
[ctpool_declare_scoped(3)](../man/cthreadpool/ctpool_declare_scoped.3),
[ctpool_construct(3)](../man/cthreadpool/ctpool_construct.3),
[ctpool_construct_scoped(3)](../man/cthreadpool/ctpool_construct_scoped.3),
[ctpool_destroy(3)](../man/cthreadpool/ctpool_destroy.3)

Submitting tasks:
[ctpool_submit(3)](../man/cthreadpool/ctpool_submit.3),
[ctpool_try_submit(3)](../man/cthreadpool/ctpool_try_submit.3),
[ctpool_timed_submit(3)](../man/cthreadpool/ctpool_timed_submit.3)

Futures: [ctpool_submit_future(3)](../man/cthreadpool/ctpool_submit_future.3),
[ctpool_try_submit_future(3)](../man/cthreadpool/ctpool_try_submit_future.3),
[ctpool_timed_submit_future(3)](../man/cthreadpool/ctpool_timed_submit_future.3),
[ctpool_future_get(3)](../man/cthreadpool/ctpool_future_get.3),
[ctpool_future_done(3)](../man/cthreadpool/ctpool_future_done.3),
[ctpool_future_cancelled(3)](../man/cthreadpool/ctpool_future_cancelled.3),
[ctpool_future_free(3)](../man/cthreadpool/ctpool_future_free.3),
[ctpool_future_create_detached(3)](../man/cthreadpool/ctpool_future_create_detached.3),
[ctpool_future_fulfill(3)](../man/cthreadpool/ctpool_future_fulfill.3)

Waiting, shutting down and inspecting:
[ctpool_wait(3)](../man/cthreadpool/ctpool_wait.3),
[ctpool_shutdown_drain(3)](../man/cthreadpool/ctpool_shutdown_drain.3),
[ctpool_shutdown_immediate(3)](../man/cthreadpool/ctpool_shutdown_immediate.3),
[ctpool_pending_count(3)](../man/cthreadpool/ctpool_pending_count.3),
[ctpool_active_count(3)](../man/cthreadpool/ctpool_active_count.3)

Related guides:
[Thread communication](cthreadcomm.md),
[Concurrency and fork()](concurrency.md),
[Memory management](memory.md),
[Design of the macros, errors and timeouts](design.md)
