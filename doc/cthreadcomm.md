# cthreadcomm: passing data between threads

If two threads read and write the same variable without any coordination you
get a data race, and that is undefined behaviour. `cthreadcomm` gives you
safe ways to hand data from one thread to another and to wait for events.
There are five pieces:

- `ccol_circular_queue`, a bounded queue whose sender waits while it's full,
  so a fast producer can only go as fast as its consumer.
- `ccol_dynamic_queue`, an unbounded queue whose sender never waits.
- `ccol_channel`, two bounded queues behind one handle, which an "owner"
  thread uses to talk to its "worker" threads.
- `ccol_select`, which waits until the first of several queues or file
  descriptors is ready. It's like `poll(2)` but it takes queues too.
- `ccol_event_loop`, a persistent reactor that runs your callbacks on its own
  thread (or threads) every time a watched queue or file descriptor is
  ready.

This module is for programs that create their own threads and need them to
communicate. If all you want is to run functions on N threads,
[cthreadpool](cthreadpool.md) manages the threads for you, and for an HTTP
service [chttpserver](chttpserver.md) already runs its own event loop.

```c
#include <ccollections/cthreadcomm.h>
```

## A first example

One thread produces numbers and the main thread consumes them. A message
without a payload marks the end of the stream:

```c
#include <ccollections/cthreadcomm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void *producer(void *arg) {
    ccol_circular_queue *q = arg;
    for (int i = 1; i <= 5; i++) {
        int *n = malloc(sizeof(*n));
        if (!n) continue;             /* no memory: ignore this number */
        *n = i * i;
        c_message_t msg = { .data = n, .size = sizeof(*n) };
        if (ccol_circq_send_zc(q, &msg) != ccol_success)
            free(msg.data);           /* after a failed send, the data is ours */
        /* after a successful send, msg.data is NULL: the queue owns the data */
    }
    c_message_t done = { .data = NULL, .size = 0 };   /* end of the stream */
    ccol_circq_send_zc(q, &done);
    return NULL;
}

int main(void) {
    char *err = NULL;
    ccol_circular_queue *q = ccol_circular_queue_create(4, &err);
    if (!q) { fprintf(stderr, "queue: %s\n", err); return 1; }

    pthread_t t;
    if (pthread_create(&t, NULL, producer, q) != 0) {
        ccol_circular_queue_destroy(q);
        return 1;
    }

    for (;;) {
        c_message_t msg;
        ccol_circq_recv_zc(q, &msg);  /* waits until a message arrives */
        if (!msg.data) break;         /* the sentinel */
        printf("got %d\n", *(int *)msg.data);
        free(msg.data);               /* the receiver owns the payload */
    }

    pthread_join(t, NULL);
    ccol_circular_queue_destroy(q);   /* the queue must be empty here */
    return 0;
}
```

Build it with `-lccollections -lpthread`.

## Messages and who owns them

Every queue carries a `c_message_t`:

```c
typedef struct c_message_t {
    void   *data;   /* your payload, usually on the heap */
    size_t  size;   /* the size of the payload in bytes */
} c_message_t;
```

The queues never copy your payload. They move the pointer, and the
**ownership** of the payload moves with it, which is what the `_zc` ("zero
copy") in the function names stands for. A successful send sets `msg.data`
to `NULL` because the queue has taken the payload over, so don't touch it
again. A failed send leaves `msg` as it was, so the payload is yours
and you have to free it or send it somewhere else. A receive hands the
payload to you, and then freeing it is your job.

Since only one thread holds a given payload at any moment, a whole class of
concurrency bugs simply can't happen. A message with `data` set to `NULL`
and `size` set to `0` is valid and makes a good "stop" sentinel, as the
example above shows. A message whose two fields disagree (a `NULL` `data`
with a `size` above `0`, or a non-`NULL` `data` with a `size` of `0`) is
refused by every send with `ccol_invalid_args`, and it stays yours.

## Choosing a queue

| You need | Use |
|---|---|
| A producer that must go slower when the consumer is slow | `ccol_circular_queue` |
| A producer that must not wait | `ccol_dynamic_queue` |
| One owner thread that sends work to helpers and gets replies | `ccol_channel` |
| One thread that waits on several queues or sockets at the same time | `ccol_select` |
| Callbacks that run each time a queue or socket is ready, for a long time | `ccol_event_loop` |

You can use any of the queues from many threads at the same time. When you
aren't sure which queue to pick, start with the circular queue: its sender
waits instead of piling up messages, so a consumer that falls behind can't
make the program run out of memory.

## Blocking, try and timed calls

Most operations come in three forms:

```c
ccol_circq_send_zc(q, &msg);                  /* waits while the queue is full */
ccol_circq_try_send_zc(q, &msg);              /* ccol_container_full immediately */
ccol_circq_timed_send_zc(q, &msg, 250000);    /* waits at most 250 ms */

ccol_circq_recv_zc(q, &msg);                  /* waits for a message */
ccol_circq_try_recv_zc(q, &msg);              /* ccol_container_empty immediately */
ccol_circq_timed_recv_zc(q, &msg, 250000);
```

Timeouts are `uint64_t` microseconds on a monotonic clock, so changing the
system time doesn't make them longer or shorter. A timed call with a timeout
of `0` behaves exactly like its `try_` form and gives the same result code
(`ccol_container_full` or `ccol_container_empty`, not `ccol_timed_out`). The
timeout conventions of the whole library are in
[the design guide](design.md).

The dynamic queue has a single send, `ccol_dynmq_send_zc`, which never
waits, and the usual blocking, try and timed receive. A channel has all six
calls, named `ccol_chan_`.

`ccol_circq_disable_sending` (and its `dynmq` and `chan` versions) closes a
queue to new sends. Senders that are waiting on a full queue then return
`ccol_not_permitted` and keep their messages, while whatever is already in
the queue stays there for the consumer. A receive on an empty queue keeps
waiting though, so the consumer needs a sentinel (or a timed receive) to find
out that the stream is over. A producer that waits with `ccol_select` (or
with an event loop write registration, see below) does find out: a queue
whose sends are off counts as ready for writing, the same way a pipe whose
reader went away counts as ready for `POLLOUT`, so the wait returns and the
producer's send then reports `ccol_not_permitted`.

## Channels: an owner and its workers

A `ccol_channel` is two circular queues behind one handle, one for each
direction. The thread that creates the channel is its **owner** and every
other thread is a **worker**. `ccol_chan_send_zc` and `ccol_chan_recv_zc`
pick the right queue from the identity of the calling thread, so what the
owner sends goes to the workers and what the workers send goes to the owner.

The channel recognises its owner by thread ID, so the owner thread must not
exit while the channel is in use. In practice that means you create the
channel on the thread that lives longest, which is usually the main thread.
There's a full example further down.

## Waiting on several sources: `ccol_select`

`ccol_select` waits until one of several **selectables** is ready and tells
you which one. You build the selectables from queues and file descriptors:

```c
size_t which;
ccol_retval_t rc = ccol_select_va(&which,
    ccol_selectable_from_circq(jobs, ccol_select_read),    /* index 0 */
    ccol_selectable_from_fd(sock_fd, ccol_select_read));   /* index 1 */
```

`ccol_select_timed_va` adds a timeout in microseconds and returns
`ccol_timed_out` when nothing becomes ready in that time. With a timeout of
`0` it checks the selectables once and doesn't wait.

The thing to remember is that **`ccol_select` only reports readiness**. It
doesn't receive or send anything for you, so after it returns you make your
own non-blocking call: `ccol_circq_try_recv_zc`, `ccol_dynmq_try_recv_zc`,
`ccol_circq_try_send_zc` or `ccol_dynmq_send_zc` for a queue, and `read(2)`
or `recv(2)` (`write(2)` or `send(2)`) for a file descriptor. Exactly as with
POSIX `select(2)`, another thread can take the message before your call
runs, which is why that call must not block and why you have to check what
it returns.

When several selectables are ready at once, they share the calls of
`ccol_select` fairly whatever their index. Each call scans the set in
rotation order from a start that moves on with every call, and a ready file
descriptor competes the same way as a ready queue. A worker that waits on a
stop pipe and a job queue therefore sees the stop byte within a few calls,
even if the queue never runs empty. Which of the ready selectables a single
call reports is not specified, so if one of them must take priority, check
it yourself before or after the call.

Every file descriptor in the set has to be something the kernel can watch: a
socket, a pipe, a FIFO, a terminal or an eventfd. A regular file or a
directory is refused.

When the set contains file descriptors, the call first asks `poll(2)`
without waiting which of them are ready, so a call that finds something
ready straight away costs one system call for all its descriptors. An epoll
instance (a kqueue on the BSDs and macOS) is only built, for the rest of the
call, when the call really has to wait.

## The persistent event loop

`ccol_select` waits once. If you watch the same sources again and again for
the whole life of a program, a `ccol_event_loop` is the better tool. You
create it once and register queues and file descriptors with callbacks, and
its background thread calls you every time a source is ready:

```c
/* The smallest useful ccol_event_loop: a callback runs on the reactor
   thread whenever a job queue has messages. */
#include <ccollections/cthreadcomm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_job(ccol_event_loop loop, ccol_event_reg reg,
                   ccol_selectable *sel, void *arg) {
    (void)loop; (void)reg;
    ccol_dynamic_queue *acks = arg;
    c_message_t msg;
    /* readiness only: receive here, and allow for an empty queue */
    while (ccol_circq_try_recv_zc(sel->cq, &msg) == ccol_success) {
        printf("job: %s\n", (char *)msg.data);
        free(msg.data);
        c_message_t ack = { .data = NULL, .size = 0 };
        ccol_dynmq_send_zc(acks, &ack);    /* tell main that one job is done */
    }
}

int main(void) {
    char *err = NULL;
    ccol_event_loop loop = ccol_event_loop_create(32, 1, 1, &err);
    if (!loop) { fprintf(stderr, "loop: %s\n", err); return 1; }

    ccol_circular_queue *jobs = ccol_circular_queue_create(64, NULL);
    ccol_dynamic_queue *acks = ccol_dynamic_queue_create(NULL);
    if (!jobs || !acks) return 1;
    ccol_event_handlers_t handlers = { .on_readable = on_job };
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(jobs, ccol_select_read),
        handlers, acks, &err);
    if (!reg) { fprintf(stderr, "add: %s\n", err); return 1; }

    const char *names[] = { "build #41", "build #42", "deploy" };
    int sent = 0;
    for (int i = 0; i < 3; i++) {
        c_message_t msg = { .data = strdup(names[i]), .size = strlen(names[i]) + 1 };
        if (ccol_circq_send_zc(jobs, &msg) == ccol_success) sent++;
        else free(msg.data);
    }

    for (int i = 0; i < sent; i++) {   /* wait until every sent job has run */
        c_message_t ack;
        ccol_dynmq_recv_zc(acks, &ack);
    }

    /* Teardown order: stop the threads, remove, empty, destroy. */
    ccol_event_loop_shutdown(loop);   /* completes the queued dispatches, joins */
    ccol_event_loop_remove(loop, reg);
    c_message_t left;
    while (ccol_circq_try_recv_zc(jobs, &left) == ccol_success) free(left.data);
    ccol_circular_queue_destroy(jobs);
    ccol_dynamic_queue_destroy(acks);
    ccol_event_loop_destroy(loop);
    return 0;
}
```

`ccol_event_loop_create` takes three numbers:

1. the size of the event batch of one wait (32 is more than enough);
2. the number of lock stripes (give `1`);
3. the number of reactor threads (give `1`; see below).

A few things about the callbacks are worth keeping in mind.

They only report readiness, just like `ccol_select`. Receive with
`ccol_circq_try_recv_zc(sel->cq, ...)` (or `sel->dq`), or call `read` or
`recv` on `sel->fd`, and be ready for that call to find nothing.

They run on the loop's thread and not on the thread that called
`ccol_event_loop_add`. With the default single reactor thread a slow
callback holds up every other registration, so hand slow work over to
another thread or to a [thread pool](cthreadpool.md).

Readiness is level-triggered. A socket that stays readable keeps triggering
calls until you read it, pause the registration or remove it, and the same
goes for end of file: a handler that reads 0 bytes has to remove (or pause)
its registration.

A callback gets its own registration as `reg`, so it can remove or pause
itself. Use that parameter and not a copy that the adding thread stores
after `ccol_event_loop_add` returns, because the callback can run before
that store happens.

The loop never runs the callback of one registration twice at the same time,
and it never runs the read and the write callback of one file descriptor at
the same time either, whatever the number of threads.

`ccol_event_loop` and `ccol_event_reg` are opaque value handles (integers),
not pointers. Compare them with `CCOL_EVENT_LOOP_INVALID` and
`CCOL_EVENT_REG_INVALID`. Both are `0`, so `if (!loop)` works.

### Removing registrations safely

You can call `ccol_event_loop_remove` from any thread, including from the
registration's own callback. Once it returns, no new callback of that
registration starts, but it doesn't wait for a callback that happens to be
running on another thread at that moment. If the `arg` of your callback has
to stay valid until that callback is done, set `on_removed` in
`ccol_event_handlers_t`. It runs exactly once, at a point where no callback
of the registration can run, and that's where you free `arg` and
close the descriptor.

The loop never closes a file descriptor or destroys a queue, that part is
yours. Remove every registration of a descriptor **before** you close it,
and remove the registrations of a queue (or destroy the loop) **before** you
destroy the queue. Destroying a queue that has a registration stops
the program with an assertion.

This teardown order always works:

1. Call `ccol_event_loop_shutdown`, which finishes the work it has already
   collected and joins the threads. From that point the loop takes no wake
   from a queue it watches, so other listeners of that queue keep getting
   every message, and a new `ccol_event_loop_add` on it is refused.
2. Remove the registrations, empty your queues and destroy them.
3. Call `ccol_event_loop_destroy`, which also runs the `on_removed` of every
   registration that is live.

### Watching both directions, changing direction, pausing

A registration watches one direction. To read and write one socket from
separate callbacks, add the socket twice, once with `ccol_select_read` and
once with `ccol_select_write`. `ccol_event_loop_modify` switches a
registration from write to read, for example after a non-blocking
`connect(2)` has completed. If you want the events to stop for a while and
come back later, say while a worker thread does blocking I/O on the socket,
pause and resume:

```c
/* fragment */
ccol_event_loop_pause(loop, reg);    /* no callbacks for reg from here on */
/* ... a different thread uses the fd directly ... */
ccol_event_loop_resume(loop, reg);   /* same registration, events again */
```

Pause, resume and modify only work on file descriptors. To change a queue
registration, remove it and add it again.

### More than one reactor thread

With `num_reactor_threads == 1` a single thread both waits and runs your
callbacks. A larger value keeps one waiting thread and adds a pool of
`num_reactor_threads - 1` threads for the callbacks, so one callback can take
a long time without holding up the others. Start with 1 and only go higher
when your callbacks really are slow. Whatever the value, exactly one thread
waits on the kernel, so there is no "thundering herd".

## Example: a team of workers

Jobs go out on a bounded queue so the producer can't get far ahead of the
workers, and results come back on an unbounded queue so a worker never waits
when it reports. Each worker stops when it receives its own sentinel.

```c
/* A fixed team of workers: jobs go out on a bounded queue, and results
   come back on an unbounded queue. */
#include <ccollections/cthreadcomm.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_WORKERS 3

/* A job goes out with its text and comes back with its word count. */
typedef struct { size_t id; char *text; size_t words; } job_t;

typedef struct {
    ccol_circular_queue *jobs;     /* bounded: it slows the producer down */
    ccol_dynamic_queue  *results;  /* unbounded: a worker does not wait */
} queues_t;

static size_t count_words(const char *s) {
    size_t n = 0;
    int in_word = 0;
    for (; *s; s++) {
        if (isspace((unsigned char)*s)) in_word = 0;
        else if (!in_word) { in_word = 1; n++; }
    }
    return n;
}

static void *worker(void *arg) {
    queues_t *qs = arg;
    for (;;) {
        c_message_t msg;
        ccol_circq_recv_zc(qs->jobs, &msg);
        if (!msg.data) return NULL;           /* one sentinel for each worker */

        job_t *job = msg.data;
        job->words = count_words(job->text);
        free(job->text);
        job->text = NULL;

        c_message_t out = { .data = job, .size = sizeof(*job) };
        if (ccol_dynmq_send_zc(qs->results, &out) != ccol_success)
            free(out.data);
    }
}

int main(void) {
    static const char *lines[] = {
        "the quick brown fox",
        "jumps over",
        "the lazy dog",
        "  pack my box with five dozen liquor jugs  ",
        "",
        "sphinx of black quartz judge my vow",
    };
    const size_t n_lines = sizeof(lines) / sizeof(lines[0]);

    queues_t qs = {
        .jobs = ccol_circular_queue_create(2, NULL),
        .results = ccol_dynamic_queue_create(NULL),
    };
    if (!qs.jobs || !qs.results) return 1;

    pthread_t team[NUM_WORKERS];
    int started = 0;
    while (started < NUM_WORKERS &&
           pthread_create(&team[started], NULL, worker, &qs) == 0)
        started++;

    size_t sent = 0;
    for (size_t i = 0; started > 0 && i < n_lines; i++) {
        job_t *job = malloc(sizeof(*job));
        if (!job) continue;                   /* no memory: ignore the line */
        job->id = i;
        job->text = strdup(lines[i]);
        job->words = 0;
        c_message_t msg = { .data = job, .size = sizeof(*job) };
        if (!job->text || ccol_circq_send_zc(qs.jobs, &msg) != ccol_success) {
            free(job->text);
            free(job);
            continue;
        }
        sent++;
    }

    size_t words[sizeof(lines) / sizeof(lines[0])] = { 0 };
    for (size_t got = 0; got < sent; got++) {
        c_message_t msg;
        ccol_dynmq_recv_zc(qs.results, &msg);
        job_t *done = msg.data;
        words[done->id] = done->words;
        free(done);
    }

    for (int i = 0; i < started; i++) {
        c_message_t stop = { .data = NULL, .size = 0 };
        ccol_circq_send_zc(qs.jobs, &stop);
    }
    for (int i = 0; i < started; i++)
        pthread_join(team[i], NULL);

    size_t total = 0;
    for (size_t i = 0; i < n_lines; i++) {
        printf("line %zu: %zu words\n", i, words[i]);
        total += words[i];
    }
    printf("total: %zu\n", total);

    ccol_circular_queue_destroy(qs.jobs);
    ccol_dynamic_queue_destroy(qs.results);
    return 0;
}
```

## Example: a helper thread with a channel

The main thread creates the channel and is therefore its owner. It sends a
request and receives the reply, while the helper thread receives requests and
sends replies with the very same calls. The helper sends back the request's
own buffer, and that costs nothing since the ownership travels with the
pointer.

```c
/* A helper thread for requests and replies on one ccol_channel. The main
   thread creates the channel, so it is the owner and the helper is a worker. */
#include <ccollections/cthreadcomm.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *slugify_worker(void *arg) {
    ccol_channel *ch = arg;
    for (;;) {
        c_message_t req;
        ccol_chan_recv_zc(ch, &req);       /* a worker receives from the owner */
        if (!req.data) return NULL;

        char *s = req.data;                /* we own it; change it in place */
        for (char *p = s; *p; p++)
            *p = isalnum((unsigned char)*p) ? (char)tolower((unsigned char)*p) : '-';

        c_message_t reply = req;           /* send the same buffer back */
        if (ccol_chan_send_zc(ch, &reply) != ccol_success)
            free(reply.data);
    }
}

static char *slugify(ccol_channel *ch, const char *title) {
    c_message_t req = { .data = strdup(title), .size = strlen(title) + 1 };
    if (ccol_chan_send_zc(ch, &req) != ccol_success) {   /* owner -> worker */
        free(req.data);
        return NULL;
    }
    c_message_t reply;
    ccol_chan_recv_zc(ch, &reply);         /* the owner receives from workers */
    return reply.data;                     /* the caller frees it */
}

int main(void) {
    ccol_channel *ch = ccol_channel_create(8, NULL);
    if (!ch) return 1;

    pthread_t helper;
    if (pthread_create(&helper, NULL, slugify_worker, ch) != 0) {
        ccol_channel_destroy(ch);
        return 1;
    }

    const char *titles[] = { "Hello, World!", "C11 Threads 101", "Zero Copy" };
    for (size_t i = 0; i < 3; i++) {
        char *slug = slugify(ch, titles[i]);
        printf("%-16s -> %s\n", titles[i], slug ? slug : "(failed)");
        free(slug);
    }

    c_message_t stop = { .data = NULL, .size = 0 };
    ccol_chan_send_zc(ch, &stop);
    pthread_join(helper, NULL);
    ccol_channel_destroy(ch);
    return 0;
}
```

## Example: commands, a stop signal and an idle timer

One thread waits on a command queue and a pipe at the same time, and does
some maintenance work whenever 100 ms pass without input. A pipe is a handy
way to wake a `ccol_select` from anywhere, a signal handler included.

```c
/* One thread waits on a command queue and a "stop" pipe at once, and does
   maintenance work whenever no input arrives for 100 ms. */
#include <ccollections/cthreadcomm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    ccol_dynamic_queue *commands;
    int stop_fd;                     /* write end of the stop pipe */
} feeder_args_t;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *feeder(void *arg) {
    feeder_args_t *fa = arg;
    const char *cmds[] = { "reload", "rotate-logs", "flush" };
    for (int i = 0; i < 3; i++) {
        sleep_ms(150);
        c_message_t msg = { .data = strdup(cmds[i]), .size = strlen(cmds[i]) + 1 };
        if (ccol_dynmq_send_zc(fa->commands, &msg) != ccol_success)
            free(msg.data);
    }
    sleep_ms(150);
    if (write(fa->stop_fd, "x", 1) != 1) perror("write");
    return NULL;
}

int main(void) {
    int pfd[2];
    if (pipe(pfd) != 0) return 1;
    ccol_dynamic_queue *commands = ccol_dynamic_queue_create(NULL);
    if (!commands) return 1;

    feeder_args_t fa = { .commands = commands, .stop_fd = pfd[1] };
    pthread_t t;
    if (pthread_create(&t, NULL, feeder, &fa) != 0) return 1;

    int ticks = 0;
    for (int running = 1; running;) {
        size_t which;
        ccol_retval_t rc = ccol_select_timed_va(&which, 100000,   /* 100 ms */
            ccol_selectable_from_dynq(commands, ccol_select_read),
            ccol_selectable_from_fd(pfd[0], ccol_select_read));

        if (rc == ccol_timed_out) {
            ticks++;                              /* maintenance when idle */
            continue;
        }
        if (rc != ccol_success) break;

        if (which == 0) {
            c_message_t msg;
            /* ccol_select only says "ready"; we must take the message */
            while (ccol_dynmq_try_recv_zc(commands, &msg) == ccol_success) {
                printf("command: %s\n", (char *)msg.data);
                free(msg.data);
            }
        } else {
            char c;
            if (read(pfd[0], &c, 1) == 1) running = 0;
        }
    }
    printf("stopped after %s idle tick(s)\n", ticks > 0 ? "some" : "no");

    pthread_join(t, NULL);
    /* A command may be queued when the select reports the stop byte. */
    c_message_t msg;
    while (ccol_dynmq_try_recv_zc(commands, &msg) == ccol_success) {
        printf("command: %s\n", (char *)msg.data);
        free(msg.data);
    }
    ccol_dynamic_queue_destroy(commands);   /* the queue must be empty here */
    close(pfd[0]);
    close(pfd[1]);
    return 0;
}
```

## Example: a TCP echo server on an event loop

A listening socket is registered for reading, and its callback accepts every
pending connection and registers it. The client callback echoes back what it
reads and removes itself at end of file, and `on_removed` closes the socket
and frees the context once no callback can use them. The program
runs a few blocking clients against its own server, so it needs nothing from
outside.

```c
/* A TCP echo server on one ccol_event_loop: a listening socket, one
   registration for each client, and on_removed to release each connection. */
#define _GNU_SOURCE
#include <ccollections/cthreadcomm.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
    size_t bytes_echoed;
} conn_t;

/* Runs once, at a point where no callback of the registration can run:
   the only safe time to close the fd and free the context. */
static void conn_release(void *arg) {
    conn_t *c = arg;
    printf("connection closed after %zu bytes\n", c->bytes_echoed);
    close(c->fd);
    free(c);
}

static void on_client(ccol_event_loop loop, ccol_event_reg reg,
                      ccol_selectable *sel, void *arg) {
    conn_t *c = arg;
    char buf[4096];
    ssize_t n = recv(sel->fd, buf, sizeof(buf), 0);
    if (n > 0) {
        /* A real server keeps the data that send() did not accept and
           watches for writability; a short echo of a few bytes always fits. */
        ssize_t w = send(sel->fd, buf, (size_t)n, MSG_NOSIGNAL);
        if (w > 0) c->bytes_echoed += (size_t)w;
        return;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    /* EOF or error: stop the watch; conn_release closes the fd later */
    ccol_event_loop_remove(loop, reg);
}

static void on_accept(ccol_event_loop loop, ccol_event_reg reg,
                      ccol_selectable *sel, void *arg) {
    (void)reg; (void)arg;
    for (;;) {
        int fd = accept4(sel->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;                    /* EAGAIN: no more connections */

        conn_t *c = calloc(1, sizeof(*c));
        if (!c) { close(fd); continue; }
        c->fd = fd;
        ccol_event_handlers_t h = { .on_readable = on_client,
                                    .on_removed = conn_release };
        if (!ccol_event_loop_add(loop, ccol_selectable_from_fd(fd, ccol_select_read),
                                 h, c, NULL)) {
            close(fd);
            free(c);
        }
    }
}

static int listen_on_loopback(unsigned short *port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(a);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(fd, 16) != 0 || getsockname(fd, (struct sockaddr *)&a, &len) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return fd;
}

/* A plain blocking client that main runs to test the server. */
static void talk(unsigned short port, const char *text) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    char buf[256] = { 0 };
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        send(fd, text, strlen(text), 0) > 0 &&
        recv(fd, buf, sizeof(buf) - 1, 0) > 0)
        printf("echoed: %s\n", buf);
    close(fd);
}

int main(void) {
    unsigned short port;
    int lfd = listen_on_loopback(&port);
    if (lfd < 0) { perror("listen"); return 1; }

    ccol_event_loop loop = ccol_event_loop_create(64, 1, 1, NULL);
    if (!loop) return 1;
    ccol_event_handlers_t lh = { .on_readable = on_accept };
    ccol_event_reg lreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(lfd, ccol_select_read), lh, NULL, NULL);
    if (!lreg) return 1;

    talk(port, "hello");
    talk(port, "event loops are fun");

    /* Stop the dispatch, then release everything: destroy runs the
       on_removed of every live registration. */
    ccol_event_loop_shutdown(loop);
    ccol_event_loop_remove(loop, lreg);
    ccol_event_loop_destroy(loop);
    close(lfd);
    return 0;
}
```

`accept4(2)` exists on Linux and the BSDs, but glibc only declares it when
`_GNU_SOURCE` is defined. How the event loop maps onto epoll on Linux and
kqueue on the BSDs is described in [Platforms](platforms.md).

## Good to know

After a failed send you are the owner. The send leaves `msg.data`
alone, so free the data or send the message again, or the memory leaks.

`ccol_circular_queue_destroy`, `ccol_dynamic_queue_destroy` and
`ccol_channel_destroy` assert when messages are left in the queue, and also
when a `ccol_select` call or an event loop registration is watching it, so
empty the queue and remove its registrations first. We'd rather stop the
program there than free a queue that still holds your payloads or that
another thread is about to touch.

In an event loop callback, use the queue calls and not the channel calls. A
callback runs on a loop thread, which a channel takes for a worker, so for a
selectable made with `ccol_selectable_from_chan` you receive with
`ccol_circq_try_recv_zc(sel->cq, ...)` and send with
`ccol_circq_try_send_zc(sel->cq, ...)`.

A loop can't be destroyed or shut down from its own callback, since both
would have to wait for the callback that is making the call. A destroy there
is a fatal error and a shutdown there returns `ccol_not_permitted`, so do
both from another thread.

Don't destroy a watched queue from one of its own callbacks either, and
don't hold a lock that one of its callbacks also takes while you destroy it,
because the destroy waits for those callbacks to finish.

A write registration can send after `ccol_event_loop_remove` returns
if its callback had started before the remove. Wait for its `on_removed`
before you empty and destroy the queue.

For `fork(2)`, either fork before you create any of these objects, or fork
and call `exec` immediately. The policy of the whole library is in
[Concurrency](concurrency.md).

## Reference

Overview: [cthreadcomm(7)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/cthreadcomm.7.html)

Circular queue:
[ccol_circular_queue_create(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circular_queue_create.3.html),
[ccol_circular_queue_create_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circular_queue_create_mp.3.html),
[ccol_circular_queue_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circular_queue_destroy.3.html),
[ccol_circq_send_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_send_zc.3.html),
[ccol_circq_try_send_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_try_send_zc.3.html),
[ccol_circq_timed_send_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_timed_send_zc.3.html),
[ccol_circq_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_recv_zc.3.html),
[ccol_circq_try_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_try_recv_zc.3.html),
[ccol_circq_timed_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_timed_recv_zc.3.html),
[ccol_circq_disable_sending(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_disable_sending.3.html),
[ccol_circq_enable_sending(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_enable_sending.3.html),
[ccol_circq_msg_count(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_circq_msg_count.3.html)

Dynamic queue:
[ccol_dynamic_queue_create(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynamic_queue_create.3.html),
[ccol_dynamic_queue_create_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynamic_queue_create_mp.3.html),
[ccol_dynamic_queue_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynamic_queue_destroy.3.html),
[ccol_dynmq_send_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynmq_send_zc.3.html),
[ccol_dynmq_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynmq_recv_zc.3.html),
[ccol_dynmq_try_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynmq_try_recv_zc.3.html),
[ccol_dynmq_timed_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynmq_timed_recv_zc.3.html),
[ccol_dynmq_disable_sending(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynmq_disable_sending.3.html),
[ccol_dynmq_enable_sending(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynmq_enable_sending.3.html),
[ccol_dynmq_msg_count(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_dynmq_msg_count.3.html)

Channel: [ccol_channel_create(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_channel_create.3.html),
[ccol_channel_create_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_channel_create_mp.3.html),
[ccol_channel_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_channel_destroy.3.html),
[ccol_chan_send_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_chan_send_zc.3.html),
[ccol_chan_try_send_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_chan_try_send_zc.3.html),
[ccol_chan_timed_send_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_chan_timed_send_zc.3.html),
[ccol_chan_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_chan_recv_zc.3.html),
[ccol_chan_try_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_chan_try_recv_zc.3.html),
[ccol_chan_timed_recv_zc(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_chan_timed_recv_zc.3.html),
[ccol_chan_disable_sending(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_chan_disable_sending.3.html),
[ccol_chan_enable_sending(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_chan_enable_sending.3.html),
[ccol_chan_msg_count(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_chan_msg_count.3.html)

Select: [ccol_select(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_select.3.html),
[ccol_select_timed(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_select_timed.3.html),
[ccol_select_va(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_select_va.3.html),
[ccol_select_timed_va(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_select_timed_va.3.html),
[ccol_selectable_from_circq(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_selectable_from_circq.3.html),
[ccol_selectable_from_dynq(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_selectable_from_dynq.3.html),
[ccol_selectable_from_chan(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_selectable_from_chan.3.html),
[ccol_selectable_from_fd(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_selectable_from_fd.3.html)

Event loop:
[ccol_event_loop_create(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_create.3.html),
[ccol_event_loop_create_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_create_mp.3.html),
[ccol_event_loop_declare(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_declare.3.html),
[ccol_event_loop_declare_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_declare_scoped.3.html),
[ccol_event_loop_construct(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_construct.3.html),
[ccol_event_loop_construct_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_construct_scoped.3.html),
[ccol_event_loop_add(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_add.3.html),
[ccol_event_loop_remove(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_remove.3.html),
[ccol_event_loop_modify(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_modify.3.html),
[ccol_event_loop_pause(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_pause.3.html),
[ccol_event_loop_resume(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_resume.3.html),
[ccol_event_loop_reg_count(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_reg_count.3.html),
[ccol_event_loop_reg_generation(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_reg_generation.3.html),
[ccol_event_loop_shutdown(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_shutdown.3.html),
[ccol_event_loop_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/cthreadcomm/ccol_event_loop_destroy.3.html)

Related guides:
[Thread pool](cthreadpool.md),
[Concurrency and fork()](concurrency.md),
[Memory management](memory.md),
[Design of the macros, errors and timeouts](design.md),
[Platforms](platforms.md),
[HTTP server](chttpserver.md)
