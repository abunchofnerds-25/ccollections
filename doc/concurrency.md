# Concurrency and fork()

This guide tells you which parts of c_collections you can share between
threads, and which parts you must protect yourself. It also tells you how to
use the library together with `fork()`.

## The rule in one sentence

**A component that moves data between threads, or that gives a shared
service, has its own locks. A component for data work in one thread has no
locks.**

| No internal locks | Internally synchronised |
|---|---|
| `cvector`, `chashmap`, `cbstmap`, `cstring`, `cjson`, `cyaml` | `cmempool` pools, the queues and channels of `cthreadcomm`, the event loop, `clrucache`, `clogger`, `cthreadpool`, `chttpclient`, `chttpserver` |

## Unguarded containers

The containers have no locks. This is intentional. A lock in each call would
not make your code safe, because real code uses calls together. Look at the
most frequent map pattern:

```c
/* Thread 1 */
if (chmap_get_ptr(map, key) == NULL)
    chmap_insert(map, key, value);

/* Thread 2, at the same time */
chmap_insert(map, key, other_value);
```

Also when each call is atomic, there is a race between the lookup and the
insert.
Thread safety is useful only at the level of your logical operation. Therefore,
put the lock at that level. Use the primitive that is correct for your
program, for example a pthread mutex, a read-write lock or C11
`<threads.h>`.

You can always do these things without a lock:

- Use **separate** containers from separate threads. Two instances share no
  state.
- Parse two **independent** JSON or YAML documents on two threads at the
  same time. Each parse writes its error message into storage of its own
  thread.

You need a lock for a container, a string or a DOM tree when two threads use
it and one or more of them writes to it.

### Example: a thread pool filling a shared map

The pool is thread-safe. Therefore, `main` submits tasks without a lock. The
map is not thread-safe. Therefore, the tasks lock a mutex around the full
sequence of the check and the action:

```c
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <ccollections/chashmap.h>
#include <ccollections/cthreadpool.h>

/* The map has no lock of its own. Therefore, each access goes through this
 * mutex. The lock covers the full check-then-act sequence, not single
 * calls. */
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
    ctpool_wait(pool);                     /* all submitted tasks ran */
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

If contention is a problem, give each worker its own map, and merge the maps
at the end. This needs no lock.

`common.h` also defines thin wrappers around pthreads (`ccol_mutex_t` and
related items). They exist to help ports of the library to other systems.
They are not part of the API. Use pthreads or `<threads.h>` directly to
protect your own data, as the example does.

## Thread-safe components

You can call these components from many threads without a lock of your own:

| Component | How it synchronises |
|---|---|
| `ccol_mempool`, `ccol_r_mempool` | an internal mutex (one for each size tier of the ranged pool) and a per-thread cache; neither exists when you create the pool for one thread |
| `ccol_circular_queue`, `ccol_dynamic_queue` | an internal mutex and condition variables |
| `ccol_channel` | two internal queues, one for each direction |
| `ccol_event_loop` | its own poller thread; see its man pages for each call |
| `clrucache` | segments that lock independently, and a condition variable for each entry |
| `clogger` | one mutex for each output target, shared by all loggers that you derive from it |
| `cthreadpool` | an internal mutex and condition variables |
| `chttpclient` | internal locks; each request call can run at the same time as each other call, and at the same time as a destroy |
| `chttpserver` | its own reactor and worker threads; see its man pages |

### Example: producers and a consumer

A queue moves the ownership of each message from the sender to the
receiver. Therefore, the threads share no other data:

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

The thread-safe components also have rules. The man page of each call gives
them. These are the rules that people most frequently do not obey:

- **The destroy is the last call, and only one thread makes it.** The
  destroy macro sets its variable to the `_INVALID` value. Therefore, a second
  destroy through that variable does nothing. But a destroy of a `ctpool`
  or `chttpclient` handle that is destroyed is a fatal error. This applies
  to a destroy through a copy of the handle, and to two destroys that race.
  `chttpclient_destroy` waits for the requests that are in progress. Therefore,
  it is safe while other threads are in a request.
- **You can shut down a pool from any location.** You can call
  `ctpool_shutdown_drain` and `ctpool_shutdown_immediate` more than one
  time and from many threads. Each call returns only after the workers
  stop. An immediate shutdown that arrives during a drain discards the
  remainder of the queue.
- **A task must not destroy its own pool.** In a task, or in its completion
  callback, `ctpool_destroy` on the same pool is fatal. In the same
  location, `ctpool_wait` returns immediately and the shutdown calls do
  nothing.
- **The LRU eviction callback runs while the cache holds a lock.** It must
  not call into the same cache again. Two segments can evict at the same
  time. Therefore, the callback can run on two threads at the same time, and it
  must protect its own state.
- **A custom allocator that you give to a thread-safe object must be
  thread-safe itself.** The reason is that the object calls the allocator
  from the thread that does the work. See [Memory management](memory.md).

## fork()

### The two supported patterns

A program that forks while it uses `cthreadpool`, `cthreadcomm`, `clogger`,
`chttpclient` or `chttpserver` must use one of two patterns:

1. **Fork first, then create.** Call `fork()` before you create a handle.
   Let each process make its own handles. This is the prefork model of nginx
   and Apache.
2. **Fork, then exec immediately.** A handle can exist when you call
   `fork()`, but the child calls `exec()` immediately. `posix_spawn()` and
   the `os/exec` package of Go do this.

All other uses are **unsupported by design**. An example is a child that
continues to run without `exec()` while a handle that you created before the
fork is live. A `fork()` copies only the thread that calls it. Therefore, the
worker, poller and writer threads of these handles do not exist in the
child, and the child cannot use the handle again. Also, the library cannot
know what other code in the process holds at the time of such a fork. Some
examples of this code are OpenSSL, the C library and other libraries.

This program uses the two patterns:

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

    /* Pattern 2: a handle exists. Therefore, the child must exec at once. */
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

By default, those five modules register `pthread_atfork()` handlers. These
handlers make sure that after a `fork()`, the child never holds an internal
lock of the library in a locked state. This is true for each thread that
used the lock at the time of the fork. The HTTP client also makes a
`fork()` wait while a teardown of its background engine is in progress.

This protection adds some work to each `fork()` in the process. Your program
possibly never forks, or it possibly forks only as in pattern 2. In these
cases, you can build the library without the protection:

```bash
make EXTRA_CFLAGS="-DCCOL_FORK_SAFETY_REQUIRED=0"
```

This changes only the fork handling. The library keeps all locks and all
other guarantees. When you remove the handlers, your program must not call
`fork()` while a different thread can be in one of those modules.

### Descriptors and exec()

Each descriptor that the library creates is closed on exec. Therefore, a
child that calls `exec()` keeps none of them.

On macOS, a socket, an accepted socket and a pipe take two calls to become
closed on exec. A `fork()` from a different thread waits until both calls
are done. The wait takes some microseconds. It does not depend on
`CCOL_FORK_SAFETY_REQUIRED`.

`posix_spawn()` runs no fork handlers, so it does not wait. On macOS, a
program that calls `posix_spawn()` while a different thread can create a
socket through the library must give `POSIX_SPAWN_CLOEXEC_DEFAULT` to
`posix_spawnattr_setflags()`. Then name each descriptor that the child must
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

[ccollections(7)](../man/common/ccollections.7) (thread-safety classes, fork policy,
CCOL_FORK_SAFETY_REQUIRED),
[ctpool_destroy(3)](../man/cthreadpool/ctpool_destroy.3),
[ctpool_wait(3)](../man/cthreadpool/ctpool_wait.3),
[ctpool_shutdown_drain(3)](../man/cthreadpool/ctpool_shutdown_drain.3),
[ctpool_shutdown_immediate(3)](../man/cthreadpool/ctpool_shutdown_immediate.3),
[chttpclient_destroy(3)](../man/chttpclient/chttpclient_destroy.3),
[clrucache_create_full(3)](../man/clrucache/clrucache_create_full.3),
[clog_derive(3)](../man/clogger/clog_derive.3),
[clog_set_level(3)](../man/clogger/clog_set_level.3),
[ccol_circq_send_zc(3)](../man/cthreadcomm/ccol_circq_send_zc.3),
[ccol_circq_recv_zc(3)](../man/cthreadcomm/ccol_circq_recv_zc.3),
[cjson_parse_mp(3)](../man/cjson/cjson_parse_mp.3)

Related guides:
[How the library works](design.md),
[Memory management](memory.md),
[cthreadpool](cthreadpool.md),
[cthreadcomm](cthreadcomm.md),
[clrucache](clrucache.md),
[clogger](clogger.md),
[chttpclient](chttpclient.md),
[chttpserver](chttpserver.md)
