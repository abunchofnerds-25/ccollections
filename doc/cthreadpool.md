# cthreadpool: running tasks on a pool of threads

A new thread for each small item of work costs much time. Each
`pthread_create` allocates a stack and makes system calls. A thread pool
starts a fixed number of worker threads one time and keeps them. You submit a
task (a function and an argument), and the next free worker runs it. The
pool sets a limit on the number of threads that run at the same time. It
also removes the cost to create a thread for each task.

`cthreadpool` gives you:

- a bounded or unbounded task queue,
- blocking, non-blocking and timed submits,
- an optional completion callback for each task,
- **futures**, which let the thread that submits a task get the `void *`
  result of that task,
- a barrier (`ctpool_wait`) and two shutdown modes.

Use it when you have independent items of work that can run in parallel. For
example: parse files, calculate the hashes of blocks, or answer requests. Use
[cthreadcomm](cthreadcomm.md) instead when you manage your own long-lived
threads and they must only exchange messages.

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
before `ccol_create_cthread_pool` returns. Therefore, the pool is ready when you
get the handle.

## Creating a pool

```c
char *err = NULL;
ctpool pool = ccol_create_cthread_pool(num_threads, queue_capacity, &err);
if (!pool) { /* err gives the cause */ }
```

`queue_capacity` selects the queue mode one time, for the life of the pool:

- **`0`** (or `ccol_invalid_size`): an unbounded queue. A submit does not
  wait for space. It fails only when no memory is available.
- **`N > 0`**: a bounded queue of `N` tasks. A plain submit waits while the
  queue is full. Therefore, a producer goes only as fast as the workers.

`ctpool` is an opaque value handle (an integer), not a pointer. Compare it
with `CTPOOL_INVALID`, which is `0`. Therefore, `if (!pool)` works. The library
examines each use of the handle. Therefore, it finds a handle whose pool you
destroyed, and it does not dereference that handle.

The macro forms declare the variable for you. They stop the program through
`ccol_fatal_err` if the creation fails. This is useful when no correct
recovery is possible:

```c
/* fragment */
ctpool_construct(pool, 4, 256);          /* you must destroy it later */
ctpool_construct_scoped(pool2, 4, 0);    /* destroyed at the end of the scope */
```

`ccol_create_cthread_pool_mp` takes a custom allocator for the memory of the
pool itself. See [Memory management](memory.md). Futures always use the
standard `malloc` and `free`.

## Submitting tasks

A task is `void fn(void *arg)`. Three submit functions cover the usual types
of producer:

```c
/* fragment */
ctpool_submit(pool, fn, arg, on_complete);              /* waits for space */
ctpool_try_submit(pool, fn, arg, on_complete);          /* ccol_container_full immediately */
ctpool_timed_submit(pool, fn, arg, on_complete, 50000); /* waits a maximum of 50 ms */
```

The timeout is a `uint64_t` count of microseconds. The library measures it on
a monotonic clock. With a timeout of `0`, the timed submit operates as a
`try` submit. For the timeout conventions of the library, see
[the design guide](design.md).

The pool does not free `arg`. It is yours. Use `on_complete` to release it.

### The completion callback

`on_complete` has the prototype `void on_complete(void *arg, bool ran)`. It
can be `NULL`. The pool calls it **exactly one time for each task that a
submit accepted**:

- `on_complete(arg, true)` on the worker, immediately after `fn` returns;
- `on_complete(arg, false)` when `ctpool_shutdown_immediate` discards the
  task before a worker took it. In that case, `fn` does not run.

Therefore, a task that owns an argument on the heap can free it in
`on_complete`, for all results of the task. A failed submit accepted nothing,
and it does not call `on_complete`. Therefore, after a failed submit, you must
free the argument.

## Futures: getting a result back

A future task is `void *fn(void *arg)`. Its return value goes back to each
thread that waits on the future:

```c
/* fragment */
ctpool_future *f = ctpool_submit_future(pool, compute, arg);
if (f) {
    void *result = ctpool_future_get(f);   /* waits until the task is complete */
    /* ... use result, then free it as compute allocated it ... */
    ctpool_future_free(f);                 /* release your handle, exactly one time */
}
```

Three rules help you to use futures correctly:

- **The result is yours.** The pool only moves the pointer. It does not copy
  the result, and it does not free it. `ctpool_future_free` frees the
  future, not the result.
- **Call `ctpool_future_free` exactly one time for each future**, also when
  you did not call `ctpool_future_get`. A free without a get means "start the
  task and ignore it". The task runs, and the pool discards its result.
- **Many threads can wait** in `ctpool_future_get` on one future, and all of
  them get the same result. It is one future. Therefore, it gets one
  `ctpool_future_free` in total.

`ctpool_future_done` examines the future and does not wait.
`ctpool_try_submit_future` and `ctpool_timed_submit_future` are the
non-blocking submit and the timed submit. They return a status code and give
the future through a pointer. Therefore, you can know the difference between a
full queue, a shutdown and no memory.

## Waiting and shutting down

`ctpool_wait` is a barrier. It returns when the queue is empty and no task runs.
The pool continues to exist. Therefore, you can submit the next phase after it:

```c
/* fragment */
for (size_t i = 0; i < n; i++) ctpool_submit(pool, phase1, &items[i], NULL);
ctpool_wait(pool);       /* all of phase 1 is complete */
for (size_t i = 0; i < n; i++) ctpool_submit(pool, phase2, &items[i], NULL);
```

Do not let a different thread submit while you wait. With an unbounded
queue, the wait can continue for all time.

There are two methods to stop a pool:

- **`ctpool_shutdown_drain`** completes all queued tasks and all running
  tasks. Then it stops the workers.
- **`ctpool_shutdown_immediate`** discards the queued tasks. Each discarded
  task gets `on_complete(arg, false)`. The pool marks each discarded future
  as cancelled: `ctpool_future_get` gives `NULL`, and
  `ctpool_future_cancelled` is true. The running tasks complete, and then
  the function stops the workers.

For all callers, both functions wait until all workers stop. After a
shutdown, a submit gives `ccol_not_permitted`, and `ctpool_submit_future`
gives `NULL`. `ctpool_destroy` frees the pool and sets the handle to
`CTPOOL_INVALID`. If you did not call a shutdown function, it completes the
queued tasks first.

## Detached futures

Sometimes a thread that is not a pool worker makes the result. For example: a
network thread, an event loop callback, or a hardware completion. A
**detached future** is the same `ctpool_future`, but you create it without a
pool. The producer calls `ctpool_future_fulfill` one time. The consumer uses
the usual `ctpool_future_get` and `ctpool_future_free`. A full example is
below.

## Example: counting words chapter by chapter

Each chapter is an independent task. A shared atomic counter collects the
total of the book. `ctpool_wait` is the barrier before the report. The pool
is scoped. Therefore, the program destroys it at the end of its block.

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
        /* Scoped: destroyed (after all tasks complete) at the end of this
           block. 0 means an unbounded queue, therefore the loop does not wait. */
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

The example divides the range into eight tasks. Each task gives a count
that it allocated on the heap. The main thread collects the futures in their
order. Therefore, the output is always the same, for all schedules of the tasks.

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
        ctpool_future_free(futures[i]);            /* exactly one time for each future */
    }
    printf("primes below %lu: %lu\n", LIMIT, total);

    ctpool_destroy(pool);
    return 0;
}
```

## Example: a bounded job queue with an emergency stop

The pool has two workers and a queue of four slots. For each job, the
producer waits a maximum of 50 ms for space. Each job owns its argument on
the heap. `on_complete` frees the argument when the job ran, and also when
the emergency stop discarded the job. Therefore, no leak occurs in either case.

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

/* Called exactly one time for each accepted job: ran == true after run_job,
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
            refused++;            /* no memory: the program does not submit this job */
            continue;
        }
        job->id = i;
        /* Wait a maximum of 50 ms for a free slot; if none, ignore this job. */
        ccol_retval_t rc = ctpool_timed_submit(pool, run_job, job, job_done, 50000);
        if (rc == ccol_success) {
            accepted++;
        } else {
            refused++;            /* the pool did not accept the job: free it */
            free(job);
        }
    }

    /* Emergency stop: running jobs complete. The pool discards the queued
       jobs and calls job_done(arg, false) for them before this call returns. */
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
    ctpool_future_fulfill(reply, strdup("HTTP/1.1 200 OK"));  /* exactly one time */
    return NULL;
}

int main(void) {
    char *err = NULL;
    ctpool_future *reply = ctpool_future_create_detached(&err);
    if (!reply) { fprintf(stderr, "future: %s\n", err); return 1; }

    pthread_t t;
    if (pthread_create(&t, NULL, network_thread, reply) != 0) {
        ctpool_future_fulfill(reply, NULL);      /* the reference of the producer */
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

`ctpool_future_fulfill` can run before or after the consumer starts to wait.
The consumer can also free its reference first. The library releases the
future when the last holder releases its reference.

## Good to know

- **Do not destroy a pool from one of its own tasks**, or from the
  `on_complete` of one of its tasks. That is a fatal error. `ctpool_wait` and
  the two shutdown functions do nothing when you call them from these
  locations, because a worker cannot wait for itself.
- **A task that submits to its own bounded pool** gets `ccol_container_full`
  when the queue is full (or `NULL` from `ctpool_submit_future`). It does not
  wait, because only the workers can make space. In that case, use
  `ctpool_try_submit`, `ctpool_timed_submit` or an unbounded queue.
- **Do not destroy a pool through an old copy of its handle.**
  `ctpool_destroy` sets your variable to `CTPOOL_INVALID`, and a second
  destroy of that variable does nothing. A destroy through a different copy
  of the same handle stops the program. It does not corrupt memory.
- **`ctpool_wait` does not wait for the callbacks of discarded tasks.** When
  a different thread calls `ctpool_shutdown_immediate`, the return of that
  call tells you that all `on_complete(arg, false)` calls are complete.
- **A custom allocator does not see one allocation for each task.** The pool
  uses its records for tasks again. Therefore, a counting allocator sees calls
  only sometimes.
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
