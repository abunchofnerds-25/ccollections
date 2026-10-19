# Concurrency and fork()

This guide is about which parts of c_collections you can share between
threads and which parts you have to protect yourself, and about using the
library together with `fork()`.

## The rule in one sentence

A component that moves data between threads, or that gives a shared service,
has its own locks, and a component that is meant for data work inside one
thread has none.

| No internal locks | Internally synchronised |
|---|---|
| `cvector`, `chashmap`, `cbstmap`, `cstring`, `cjson`, `cyaml` | `cmempool` pools, the queues and channels of `cthreadcomm`, the event loop, `clrucache`, `clogger`, `cthreadpool`, `chttpclient`, `chttpserver` |

## Unguarded containers

The containers have no locks on purpose. A lock inside each call wouldn't
make your code safe, because real code combines calls. Take the most common
map pattern:

```c
/* Thread 1 */
if (chmap_get_ptr(map, key) == NULL)
    chmap_insert(map, key, value);

/* Thread 2, at the same time */
chmap_insert(map, key, other_value);
```

Even if each call were atomic, the lookup and the insert would still race.
Thread safety only helps at the level of your logical operation, so that's
where the lock goes, and you can use whatever primitive suits your program:
a pthread mutex, a read-write lock or C11 `<threads.h>`.

Using separate containers from separate threads is always safe without a
lock, because two instances share no state. So is parsing two independent
JSON or YAML documents on two threads at the same time, because each parse
writes its error message into storage that belongs to its own thread. You
need a lock for a container, a string or a DOM tree when two threads use it
and at least one of them writes to it.

### Example: a thread pool filling a shared map

The pool is thread-safe, so `main` submits tasks without a lock. The map
isn't, so the tasks lock a mutex around the whole check-and-act sequence:

```c
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <ccollections/chashmap.h>
#include <ccollections/cthreadpool.h>

/* The map has no lock of its own, so every access goes through this
 * mutex. */
static pthread_mutex_t counts_lock = PTHREAD_MUTEX_INITIALIZER;
static chmap counts;

struct job { const char *line; };

static void count_line(void *arg) {
    chmap_redeclare(counts, char *, int);
    struct job *job = arg;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", job->line);
    for (char *save = NULL, *w = strtok_r(buf, " ", &save); w;
         w = strtok_r(NULL, " ", &save)) {
        pthread_mutex_lock(&counts_lock);
        int *n = chmap_get_ptr(counts, w);
        if (n)
            (*n)++;
        else
            chmap_insert(counts, w, 1);
        pthread_mutex_unlock(&counts_lock);
    }
}

int main(void) {
    static struct job jobs[] = {
        { "the quick brown fox" }, { "jumps over the lazy dog" },
        { "the dog sleeps" },      { "the fox runs" },
    };
    chmap_construct(map, char *, int);
    counts = map;

    ctpool_construct(pool, 4, 0);          /* 4 workers, unbounded queue */
    int failed = 0;
    for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]) && !failed; i++)
        failed = ctpool_submit(pool, count_line, &jobs[i], NULL) != ccol_success;
    ctpool_wait(pool);                     /* every submitted task has run */
    ctpool_destroy(pool);
    if (failed) {
        chmap_destroy(map);
        return 1;
    }

    printf("the=%d fox=%d dog=%d\n", chmap_get(map, "the"),
           chmap_get(map, "fox"), chmap_get(map, "dog"));
    chmap_destroy(map);
    return 0;
}
```

If the contention starts to hurt, give each worker its own map and merge
the maps at the end, which needs no lock at all.

`common.h` also has thin wrappers around pthreads (`_ccol_mutex_t` and
friends). They are there to make the library easier to port to other
systems and they aren't part of the API, so protect your own data with
pthreads or `<threads.h>` directly like the example does.

## Thread-safe components

You can call these components from many threads without a lock of your
own:

| Component | How it synchronises |
|---|---|
| `ccol_mempool`, `ccol_r_mempool` | an internal mutex (one for each size tier of the ranged pool) and a per-thread cache; neither exists when you create the pool for a single thread |
| `ccol_circular_queue`, `ccol_dynamic_queue` | an internal mutex and condition variables |
| `ccol_channel` | two internal queues, one for each direction |
| `ccol_event_loop` | its own poller thread; see its man pages for each call |
| `clrucache` | segments that lock independently, and a condition variable for each entry |
| `clogger` | one mutex for each output target, shared by all loggers that you derive from it |
| `cthreadpool` | an internal mutex and condition variables |
| `chttpclient` | internal locks; any request call can run at the same time as any other call, including a destroy |
| `chttpserver` | its own reactor and worker threads; see its man pages |

### Example: producers and a consumer

A queue passes the ownership of each message from the sender to the
receiver, so the threads don't share any other data:

```c
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ccollections/cthreadcomm.h>

#define PRODUCERS 3
#define PER_PRODUCER 5

static ccol_circular_queue *queue;   /* thread-safe: no lock needed */

static void *producer(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < PER_PRODUCER; i++) {
        char *text = malloc(32);
        if (!text) abort();
        snprintf(text, 32, "producer %d item %d", id, i);
        c_message_t msg = { text, strlen(text) + 1 };
        if (ccol_circq_send_zc(queue, &msg) != ccol_success)
            free(text);                  /* after a failed send, we own it */
    }
    return NULL;
}

int main(void) {
    queue = ccol_circular_queue_create(4, NULL);
    if (!queue) return 1;

    pthread_t tids[PRODUCERS];
    int started = 0;
    for (long i = 0; i < PRODUCERS; i++)
        if (pthread_create(&tids[started], NULL, producer, (void *)i) == 0)
            started++;

    for (int got = 0; got < started * PER_PRODUCER; got++) {
        c_message_t msg;
        if (ccol_circq_recv_zc(queue, &msg) != ccol_success) break;
        puts((char *)msg.data);
        free(msg.data);                  /* the receiver owns the data */
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);

    ccol_circular_queue_destroy(queue);  /* the queue is empty here */
    return 0;
}
```

## Rules that you must also obey

The thread-safe components have rules as well, and the man page of each
call states them. These are the ones people break most often.

The destroy is the last call, and only one thread makes it. The destroy
macro sets its variable to the `_INVALID` value, so a second destroy through
the same variable does nothing, but destroying a `ctpool` or `chttpclient`
handle that's already destroyed is a fatal error, whether it happens through
a copy of the handle or through two destroys that race.
`chttpclient_destroy` waits for the requests in progress, so you can call it
while other threads are inside a request.

You can shut a pool down from anywhere. `ctpool_shutdown_drain` and
`ctpool_shutdown_immediate` can be called more than once and from many
threads, and each call only returns once the workers have stopped. An
immediate shutdown that comes in during a drain discards the rest of the
queue.

A task must not destroy its own pool. Inside a task or its completion
callback, `ctpool_destroy` on the same pool is fatal, while `ctpool_wait`
returns at once and the shutdown calls do nothing.

The LRU eviction callback runs while the cache holds a lock, so it must not
call back into the same cache, and it's best to keep it short, since other
calls on that part of the cache wait for it. Two segments can evict at the
same time, so the callback can also run on two threads at once and has to
protect its own state.

A custom allocator that you give to a thread-safe object has to be
thread-safe itself, because the object calls it from whatever thread does
the work. See [Memory management](memory.md).

## fork()

### The two supported patterns

A program that forks while it uses `cthreadpool`, `cthreadcomm`,
`clogger`, `chttpclient` or `chttpserver` has to follow one of two
patterns:

1. Fork first, then create. You call `fork()` before you create any handle
   and each process makes its own handles. This is the prefork model of
   nginx and Apache.
2. Fork, then exec at once. A handle may exist when you call `fork()`, but
   the child calls `exec()` right away, which is how `posix_spawn()` and
   Go's `os/exec` package work.

Anything else is unsupported by design, for example a child that keeps
running without `exec()` while a handle created before the fork is live. A
`fork()` only copies the calling thread, so the worker, poller and writer
threads behind these handles don't exist in the child and the child can't
use the handle again. On top of that, the library can't know what other
code in the process (OpenSSL, the C library or some other library) holds at
the moment of such a fork.

This program shows both patterns:

```c
#include <stdatomic.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ccollections/cthreadpool.h>

static atomic_int done;

static void work(void *arg) { (void)arg; atomic_fetch_add(&done, 1); }

/* Pattern 1: fork first, then create handles in each process. */
static int run_worker(int id) {
    ctpool_construct(pool, 2, 0);       /* the pool of this process */
    int failed = 0;
    for (int i = 0; i < 10 && !failed; i++)
        failed = ctpool_submit(pool, work, NULL, NULL) != ccol_success;
    ctpool_destroy(pool);                /* runs all queued tasks first */
    if (failed) return 1;
    printf("worker %d ran %d tasks\n", id, atomic_load(&done));
    fflush(stdout);                      /* _exit() does not flush */
    return 0;
}

int main(void) {
    for (int id = 0; id < 2; id++) {
        pid_t pid = fork();              /* no handle exists yet */
        if (pid < 0) return 1;
        if (pid == 0) _exit(run_worker(id));
    }
    while (wait(NULL) > 0) {}

    /* Pattern 2: a handle exists, so the child must exec at once. */
    ctpool_construct(pool, 2, 0);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        execlp("echo", "echo", "child replaced its image", (char *)NULL);
        _exit(127);                      /* exec failed: exit; use nothing */
    }
    if (pid > 0) waitpid(pid, NULL, 0);
    ctpool_destroy(pool);
    return 0;
}
```

### What the library does for you

By default those five modules register `pthread_atfork()` handlers so
that after a `fork()` the child never inherits one of the library's
internal locks in a locked state, whatever thread was using the lock when
the fork happened. The HTTP client also makes a `fork()` wait until a
teardown of its background engine is finished and the thread that ran it
has ended, so no thread of the client is running at the fork.

This protection adds some work to every `fork()` in the process. If your
program never forks, or only forks as in pattern 2, you can build the
library without it:

```bash
make EXTRA_CFLAGS="-DCCOL_FORK_SAFETY_REQUIRED=0"
```

Only the fork handling changes, and the library keeps all its locks and
all its other guarantees. Without the handlers though, your program must not
call `fork()` while another thread may be inside one of those modules, and
that includes the thread on which the HTTP client stops its engine after you
destroy the last client, so call `chttpcli_engine_wait()` before such a
`fork()`.

### Descriptors and exec()

Every descriptor that the library creates is closed on exec, so a child
that calls `exec()` keeps none of them. That includes the system CA bundle,
which the HTTP client reads once per process through such a descriptor. The
one exception is a system with no default CA file. There the client trusts
OpenSSL's hashed certificate directory, and OpenSSL opens the files in it
during a handshake with descriptors that an `exec()` keeps (see
[chttpclient_set_tls(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpclient/chttpclient_set_tls.3.html)).

On macOS a socket, an accepted socket and a pipe each take two calls to
become closed on exec, and a `fork()` from another thread waits until both
calls are done. The wait is a few microseconds and has nothing to do with
`CCOL_FORK_SAFETY_REQUIRED`.

`posix_spawn()` runs no fork handlers, so it doesn't wait. On macOS a
program that calls `posix_spawn()` while another thread may be creating a
socket through the library must pass `POSIX_SPAWN_CLOEXEC_DEFAULT` to
`posix_spawnattr_setflags()` and then name each descriptor the child has to
keep with `posix_spawn_file_actions_addinherit_np()`:

```c
#include <spawn.h>

extern char **environ;

/* Runs argv[0] with only standard input, output and error. */
static int spawn_clean(pid_t *pid, char *const argv[]) {
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    for (int fd = 0; fd <= 2; fd++)
        posix_spawn_file_actions_addinherit_np(&fa, fd);
    int rc = posix_spawnp(pid, argv[0], &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    return rc;
}
```

## Reference

[ccollections(7)](https://abunchofnerds-25.github.io/ccollections/man/common/ccollections.7.html) (thread-safety classes, fork
policy,
CCOL_FORK_SAFETY_REQUIRED),
[ctpool_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadpool/ctpool_destroy.3.html),
[ctpool_wait(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadpool/ctpool_wait.3.html),
[ctpool_shutdown_drain(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadpool/ctpool_shutdown_drain.3.html),
[ctpool_shutdown_immediate(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadpool/ctpool_shutdown_immediate.3.html),
[chttpclient_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpclient/chttpclient_destroy.3.html),
[clrucache_create_full(3)](https://abunchofnerds-25.github.io/ccollections/man/clrucache/clrucache_create_full.3.html),
[clog_derive(3)](https://abunchofnerds-25.github.io/ccollections/man/clogger/clog_derive.3.html),
[clog_set_level(3)](https://abunchofnerds-25.github.io/ccollections/man/clogger/clog_set_level.3.html),
[ccol_circq_send_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_send_zc.3.html),
[ccol_circq_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_recv_zc.3.html),
[cjson_parse_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/cjson/cjson_parse_mp.3.html)

Related guides:
[How the library works](design.md),
[Memory management](memory.md),
[cthreadpool](cthreadpool.md),
[cthreadcomm](cthreadcomm.md),
[clrucache](clrucache.md),
[clogger](clogger.md),
[chttpclient](chttpclient.md),
[chttpserver](chttpserver.md)
