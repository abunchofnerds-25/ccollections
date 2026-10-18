# clogger: structured logging

`clogger` writes log records that a program can search, filter and collect.
A record isn't free-form text but a sequence of `key=value` pairs: the time,
the level, the process and the thread, the source location, the fields you
attached, and finally the message.

```
ts=2026-05-29T21:52:39.096473Z level=INFO proc=myapp(1234):main(1234) src=main.c:9 func=main env=prod msg="starting up"
```

It's a good fit when you want logs that `grep`, `jq` or a log collector can
read without guessing, when several threads log at the same time and their
lines must not get mixed up, when you want rotation of log files or logs
that go to the system's syslog daemon without writing that code yourself, or
when you want error records to carry a backtrace.

For a short single-threaded tool that prints a handful of messages,
`fprintf(stderr, ...)` is simpler and perfectly fine.

```c
#include <ccollections/clogger.h>
```

## A first logger

```c
#include <ccollections/clogger.h>

int main(void) {
    /* A logger that writes to stderr (fd 2), synchronously. */
    clog lg = clog_create_from_fd(2, CLOG_INFO, NULL);
    if (lg == CLOG_INVALID)
        return 1;

    /* Fields that every later record of this logger carries. */
    clog_set_field(lg, "service", "billing");
    clog_set_field(lg, "env", "dev");

    clog_info(lg, "starting up");
    clog_debug(lg, "you will not see this: DEBUG is below INFO");
    clog_warn(lg, "config key %s missing, using %d", "timeout", 30);
    clog_error(lg, "payment gateway returned %d", 503);

    clog_close(lg);
    return 0;
}
```

Build it with `-rdynamic` so that the backtraces show function names:

```sh
gcc -std=gnu11 -rdynamic first.c -lccollections -o first
```

The output looks like this (the order of the fields can differ):

```
ts=2026-10-06T16:24:43.551077Z level=INFO proc=first(2741242):first(2741242) src=first.c:13 func=main env=dev service=billing msg="starting up"
ts=2026-10-06T16:24:43.551121Z level=WARN proc=first(2741242):first(2741242) src=first.c:15 func=main env=dev service=billing msg="config key timeout missing, using 30"
ts=2026-10-06T16:24:43.551276Z level=ERROR proc=first(2741242):first(2741242) src=first.c:16 func=main env=dev service=billing msg="payment gateway returned 503"
	#0 ./first(main+0x186) [0x55b9358482ef]
	#1 /lib/x86_64-linux-gnu/libc.so.6(+0x29ca8) [0x7fa19bfe7ca8]
	...
```

`clog` is a handle, a plain number and not a pointer, and `CLOG_INVALID`
(zero) means "no logger". Never cast a `clog` to `void *` or back: it's a
64-bit number, and on a 32-bit system a pointer can't even hold it.

The `clog_*` macros take a `printf` format and record the file, the line and
the function for you. `clog_error` also adds a backtrace as lines that start
with a tab. Those lines don't start with `ts=`, so a log collector can
attach them to the record above them.

`clog_close` frees the logger but doesn't close fd 2. A logger never closes a
descriptor that you gave it.

## Reading a record

Each record starts with the same keys:

| Key | Meaning |
|---|---|
| `ts` | UTC time in ISO 8601, with microseconds |
| `level` | `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `ALERT` or `FATAL` |
| `proc` | `name(pid):thread(tid)`: the program name and the process ID, then the thread name and the thread ID |
| `src` | `file:line` of the log call |
| `func` | the function that wrote the record |

Your fields come after them, then `msg`. A value that contains a space, `=`,
`"`, a backslash, a control character or a byte that isn't valid UTF-8 is
written in double quotes with backslash escapes, so even a message with a
newline in it produces exactly one line. A C1 control (U+0080 to U+009F,
which a terminal may take as a control sequence) is written as `\u0080` to
`\u009f`, and every invalid byte becomes U+FFFD:

```
msg="two\nlines \"quoted\" back\\slash"
```

## Levels

From the most detailed to the most severe:

| Level | Use it for |
|---|---|
| `CLOG_TRACE` | detail for each step, while you look for the cause of a problem |
| `CLOG_DEBUG` | detail that a developer wants and an operator does not want |
| `CLOG_INFO` | usual events; the usual minimum in production |
| `CLOG_WARN` | an unusual condition that the program managed |
| `CLOG_ERROR` | a failure; adds a backtrace |
| `CLOG_ALERT` | an operator must act immediately; adds a backtrace |
| `CLOG_FATAL` | the program cannot continue; adds a backtrace, then exits |

Each level has its macro: `clog_trace`, `clog_debug`, `clog_info`,
`clog_warn`, `clog_error`, `clog_alert` and `clog_fatal`.

You pick the minimum level when you open the logger, and `clog_set_level`
changes it whenever you like. A record below the minimum is dropped and costs
almost nothing. With `CLOG_OFF` as the minimum the logger writes nothing but
`CLOG_FATAL` records.

`clog_fatal` is the odd one. It always writes whatever the minimum is, and
then ends the process with `exit(EXIT_FAILURE)`, so it never returns. Before
exiting it makes sure the records the program wrote earlier reach the log as
well, so you don't lose the last words of a dying program.

## Fields

A field is a `key=value` pair that the logger adds to every record once you
set it. They're meant for context that is the same across many records, like
a service name, a request ID or a user.

```c
#include <ccollections/clogger.h>

int main(void) {
    clog lg = clog_create_from_fd(1, CLOG_INFO, NULL);
    if (lg == CLOG_INVALID)
        return 1;

    clog_set_field(lg, "request_id", "abc-123");
    clog_info(lg, "processing");

    clog_set_field(lg, "request_id", "abc-124");   /* replaces the value */
    clog_set_field(lg, "path", "/tmp/my file");    /* in quotes in the output */
    clog_info(lg, "processing");

    clog_remove_field(lg, "request_id");
    clog_info(lg, "between requests");

    clog_clear_fields(lg);
    clog_info(lg, "no fields at all");

    clog_close(lg);
    return 0;
}
```

Leaving out the `ts` and `proc` keys, the output is:

```
level=INFO src=fields.c:9 func=main request_id=abc-123 msg=processing
level=INFO src=fields.c:13 func=main path="/tmp/my file" request_id=abc-124 msg=processing
level=INFO src=fields.c:16 func=main path="/tmp/my file" msg="between requests"
level=INFO src=fields.c:19 func=main msg="no fields at all"
```

The logger copies the key and the value, so you can free or reuse your
strings as soon as the call returns.

A key must be plain printable ASCII without spaces, `=`, `]`, `"` or `\`,
and it can't be one of the fixed names (`ts`, `level`, `proc`, `src`,
`func`, `msg`, `bt`, `bt_error`). `clog_set_field` returns
`ccol_invalid_args` for a bad key or a NULL value and
`ccol_not_enough_memory` when it can't copy the strings, and in both cases
the fields are left as they were. Whoever reads the log later depends on
the fields, so check the result where a missing one would hurt:

```c
if (clog_set_field(req_log, "request_id", id) != ccol_success)
    clog_warn(req_log, "request_id field not set");   /* fragment */
```

## Derived loggers: one logger per task

Often you want some extra fields for one item of work, one HTTP request or
one order, without touching the fields that the rest of the program uses.
`clog_derive` gives you a second handle on the same output:

```c
clog req_log = clog_derive(app_log);      /* fragment */
if (req_log == CLOG_INVALID)
    return;                               /* no memory */
clog_set_field(req_log, "request_id", id);
clog_info(req_log, "handling %s", path);
clog_close(req_log);                      /* app_log stays open */
```

A derived logger starts with a copy of its parent's fields and level, and
after that the two are independent, so a field you set on one doesn't show
up on the other. Everything about the output is shared though: the file or
descriptor, the format, the rotation, and the lock that keeps lines from
mixing. A write through any of the handles rotates the file when a limit is
reached, and rotation goes on as long as any handle is open, even after the
root is closed. The output itself stays open until the root and every
derived handle are closed. The order example below shows this in practice.

## Output formats

`clog_set_format` changes the format of a logger at any time, and the
change applies to every handle that shares the same output.

**logfmt** (`CLOG_FMT_LOGFMT`, the default) is what the examples above use.
A person can read it easily and a program can parse it easily.

**JSON** (`CLOG_FMT_JSON`) writes one JSON object per line (NDJSON). Your
fields become top-level keys and a backtrace becomes a `"bt"` array in the
same object:

```
{"ts":"2026-10-06T16:24:58.272421Z","level":"INFO","proc":"app(2741544):app(2741544)","src":"app.c:10","func":"main","env":"prod","msg":"starting up"}
{"ts":"2026-10-06T16:24:58.272599Z","level":"ERROR","proc":"app(2741544):app(2741544)","src":"app.c:11","func":"main","env":"prod","msg":"db failed: timeout","bt":["#0 ./app(main+0xf5) [0x56479704526e]","#1 ..."]}
```

Every string in a JSON record is valid UTF-8, even if you log binary data,
because each invalid byte is replaced with U+FFFD and every control
character is escaped, C1 controls included. So it's safe to pipe the output
into `jq` or a JSON collector. The logfmt and syslog formats do the same
replacement, which means every format writes valid UTF-8.

**Syslog** (`CLOG_FMT_SYSLOG`) writes RFC 5424 messages for the system log
daemon. It only works on a logger you created with `clog_create_from_fd`; on
a file logger the call returns `ccol_invalid_args` and changes nothing. On
Linux you connect a datagram socket to `/dev/log`:

```c
#include <ccollections/clogger.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return 1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strncpy(sa.sun_path, "/dev/log", sizeof(sa.sun_path) - 1);
    clog lg = CLOG_INVALID;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
        perror("connect /dev/log");
    else
        lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
    if (lg == CLOG_INVALID) {
        close(fd);
        return 1;
    }
    if (clog_set_format(lg, CLOG_FMT_SYSLOG) != ccol_success ||
        clog_set_facility(lg, CLOG_SYSLOG_DAEMON) != ccol_success) {
        clog_close(lg);
        close(fd);
        return 1;
    }

    clog_info(lg, "service started");

    clog_close(lg);   /* does not close fd */
    close(fd);
    return 0;
}
```

The daemon receives:

```
<30>1 2026-10-06T16:24:58.377293Z myhost myservice 2741559 INFO [ccol proc="myservice(2741559):myservice(2741559)" src="svc.c:26" func="main"] service started
```

Keep syslog messages under about 2 KiB, which is what a typical syslog
daemon accepts in one datagram.

## Logging to a file, with rotation

`clog_create_from_file` opens (or creates) a file and appends to it. Give it
a `clog_rotation_cfg_t` to rotate the file, or `NULL` for a plain file that
only grows:

```c
clog_rotation_cfg_t cfg = {                          /* fragment */
    .size_rotation_enabled = true,
    .max_file_size         = 50 * 1024 * 1024,      /* 50 MiB */
    .time_rotation_enabled = true,
    .rotation_interval_us  = 86400000000ULL,        /* one day */
    .max_rotated_files     = 7,                     /* keep one week */
    .compress_rotated      = true,                  /* gzip old files */
};
clog lg = clog_create_from_file("/var/log/myapp/app.log", CLOG_INFO, &cfg,
                                NULL);
```

On disk, a rotation renames `app.log` to `app.log.<YYYYMMDDHHMMSS>` and
starts a new `app.log`. A second rotation within the same second gets
`_0001`, a third one `_0002`, and so on.

Time rotation counts the interval from the last rotation. If the system
clock steps back to a moment before that rotation, the interval starts over
from the new time, so the next rotation comes one interval later.

The logger keeps the newest `max_rotated_files` old files and deletes the
rest. A value left at zero takes its default (10 MiB, one day, 7 files),
and `clogger.h` names these defaults `CLOG_DEFAULT_MAX_FILE_SIZE`,
`CLOG_DEFAULT_ROTATION_INTERVAL_US` and `CLOG_DEFAULT_MAX_ROTATED_FILES`.
You can't turn the deletion off, and that's on purpose, so old logs never
fill up the disk.

With `compress_rotated` every old file becomes a `.gz` file that `zcat` can
read. Compression runs on a background thread of the logger, so a log call
never waits for gzip. If you link the static library, link zlib (`-lz`) as
well.

A new `app.log` gets the permissions and the owner of the file it replaces,
so a log that you restricted to `0600` stays that way. The logger also works
relative to the directory where it opened the file, so a relative path keeps
working after your program calls `chdir`.

The logger only renames, compresses and deletes files that it recognises as
its own rotated files, and leaves everything else in the directory alone. A
rotated name whose timestamp is more than 30 days ahead of both the clock
and the newest name the logger made isn't one of its files: the logger
leaves it alone and logs a `WARN` record that names it. The consequence is
that if the system clock steps back by more than 30 days, a logger that
opens the file after the step (a restarted process, for example) won't
manage the old files written before the step until the clock catches up,
and you have to remove or rename them yourself.

An async logger (see the next section) checks the size after each batch it
writes, so its file can go past `max_file_size` by up to
`flush_buffer_size` bytes before it rotates. If the size of each file
matters to you, keep `flush_buffer_size` small compared to
`max_file_size`.

The rotation example below shows all of this on a real directory.

**One file, one logger.** If two parts of your program call
`clog_create_from_file` on the same rotating file, the second call joins the
first logger the way `clog_derive` does, so the two never both rename the
file. The second call fails with `CLOG_INVALID` if its rotation, async or
allocator settings differ from the first one. Open a rotating file under one
name only (a symbolic link to it counts as a second file), and never let two
processes rotate the same file, because each would rename and delete files
that the other one is writing. In a prefork server, give every worker its
own file. The details are in
[clog_create_from_file_mp(3)](../man/clogger/clog_create_from_file_mp.3).

## Async logging

By default a log call formats and writes its record before it returns. That
is simple and safe, but a slow disk or a busy pipe then slows down the
thread that logs. Synchronous mode is the right place to start for most
programs, since a record that a call has written can't be lost if the
program crashes later; async mode is worth it once the writes really hold
your threads back.

Passing a `clog_async_cfg_t` puts a logger into async mode. The logging
thread then only formats the message and hands the record over to the
logger's writer thread, which collects records and writes them in batches:

```c
clog_async_cfg_t acfg = {                     /* fragment */
    .queue_size        = 4096,                /* 0 = unbounded */
    .flush_buffer_size = 64 * 1024,           /* write at 64 KiB buffered */
    .flush_interval_us = 200000,              /* ... or every 200 ms */
};
clog lg = clog_create_from_file("app.log", CLOG_INFO, NULL, &acfg);
```

`queue_size` sets the backpressure. Zero means an unbounded queue that never
blocks a caller, while a positive size makes the caller wait when the queue
is full. The logger doesn't drop a record in either case.

The writer thread writes a batch once it reaches `flush_buffer_size`, and at
least once every `flush_interval_us`, also while records keep arriving
without a pause. When there's nothing to write it sleeps until the next
record or request, so an idle logger causes no wakeups.

Any pointer that isn't NULL puts the logger into async mode, a pointer to a
struct of zeros included, which selects the defaults (64 KiB and 200 ms).
Only `NULL` means synchronous. Every logger derived from an async logger is
async too.

`clog_flush` waits until every record logged before the call has been handed
to the operating system.

What happens at exit depends on how the process ends. `clog_close`, a normal
`exit()` and a return from `main` write the records that are left in the
queue, and the exit path waits at most 5 seconds for that. `clog_fatal`
first writes the records that earlier calls queued, then its own record, and
then exits. `_exit()`, `abort()` and a crash on a signal run no exit
handlers, so the queued records are lost, and where that matters you should
call `clog_flush` first.

`exit()` writes the queued records but doesn't stop the writer thread or the
compressor thread, which simply end with the process. That's why a leak
checker such as valgrind reports their stacks as "possibly lost" when the
program exits without `clog_close`. If you need a clean leak report, in a
test suite for example, call `clog_close` on every async or compressing
logger before you exit.

## Thread names

The `proc` key carries the name of the thread that wrote the record. By
default that's the name the operating system gives the thread, which for a
new thread is usually the program name, so it pays to give your threads
meaningful names:

```c
static void *worker(void *arg) {                     /* fragment */
    (void)arg;
    ccol_set_thread_name("ingest-worker");
    clog_info(lg, "started");   /* proc=myapp(1234):ingest-worker(1240) */
    return NULL;
}
```

`ccol_set_thread_name` keeps the first 15 bytes of the name (the Linux
limit). It needs no logger and renames only the calling thread. For a thread
that writes log records, use it instead of `pthread_setname_np`, because the
logger reads a thread's name once, at that thread's first record, and
doesn't see a rename that any other function makes afterwards.

## Backtraces

`clog_error`, `clog_alert` and `clog_fatal` add a backtrace, and you need to
link with `-rdynamic` to see function names in it. Capturing works on glibc,
FreeBSD, OpenBSD and macOS, but musl has no `backtrace()`. When the logger
can't get a backtrace, the record says so (`#error backtrace unavailable`,
or a `"bt_error"` key in JSON) instead of quietly leaving it out.

## Signals: SIGPIPE and SIGXFSZ

A logger can write to any kind of descriptor: a pipe into another program, a
socket to a log collector, a terminal or a file. Two kinds of write can
raise a signal whose default action kills the process. Writing to a pipe
with no reader raises `SIGPIPE`. Writing to a file that has reached the file
size limit of the process (`ulimit -f`, or `LimitFSIZE=` in a systemd unit)
raises `SIGXFSZ`, and every later write to that file raises it again, the
newline and the loss marker that the logger writes to repair the file
included.

`clogger` prevents both by default. A write to a broken pipe or socket is
just dropped. When a logger opens a regular file, and when a rotation opens
the next one, the library sets the disposition of `SIGXFSZ` to `SIG_IGN` for
the process, but only while the disposition is the default one, so a handler
or an ignore that your application installed is kept. A write at the limit
then fails with `EFBIG`, and the logger repairs the file as soon as there is
room again, after a rotation for example, or once the limit is raised. The
background threads of a logger also block both signals in their own signal
masks.

If your application has to manage these signals itself, call
`clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED)` before the first logger
opens. The library then changes no disposition and no signal mask, and a
program that runs under a file size limit has to ignore `SIGXFSZ`, or block
it in every thread that can write a log file, or the kernel ends the process
when a log file reaches the limit. Ignoring it is the safer choice, because
FreeBSD raises `SIGXFSZ` on the whole process, where any thread that doesn't
block it can take it.
[clog_set_signal_policy(3)](../man/clogger/clog_set_signal_policy.3)
describes both policies.

## Custom allocators and fork

`clog_create_from_fd_mp` and `clog_create_from_file_mp` take a
`ccol_memmgmt_procs_t` as their last argument, and the logger then allocates
through your functions. Set all four function pointers. Derived loggers use
the same allocator. Two allocations belong to the library and go through
`malloc`: the symbol text of a backtrace, which `backtrace_symbols` makes,
and the small identity cache that every thread fills on its first record and
that all loggers share. See [Memory management](memory.md).

If your program calls `fork()`, fork before you open a logger, or open the
logger and call `exec` in the child right after the fork. The policy behind
this is explained in [Concurrency](concurrency.md).

## Example: per-order logging in a rotating file

A small shop processes customer orders. Every record about an order carries
the order ID and the customer, and no other record of the program does. The
log goes into a temporary directory that the program removes at the end.

```c
/* orders.c: one derived logger for each customer order, in a rotating file. */
#include <ccollections/clogger.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static clog g_log;   /* the root logger */

static int ship_item(const char *item) {
    return strcmp(item, "desk lamp") == 0 ? 42 : 0;   /* the shop has no lamps */
}

static void process_order(const char *order_id, const char *customer,
                          const char *item) {
    clog order_log = clog_derive(g_log);
    if (order_log == CLOG_INVALID)
        return;

    clog_set_field(order_log, "order_id", order_id);
    clog_set_field(order_log, "customer", customer);

    clog_info(order_log, "order received: %s", item);
    int rc = ship_item(item);
    if (rc != 0)
        clog_warn(order_log, "shipping failed with code %d", rc);
    else
        clog_info(order_log, "order shipped");

    clog_close(order_log);   /* the root logger stays open */
}

/* Remove each entry of a flat directory, then the directory itself. */
static void remove_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char path[4096];
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

int main(void) {
    char dir[] = "/tmp/orders-XXXXXX";
    if (mkdtemp(dir) == NULL)
        return 1;
    char path[4096];
    snprintf(path, sizeof(path), "%s/orders.log", dir);

    clog_rotation_cfg_t rot = {
        .size_rotation_enabled = true,
        .max_file_size         = 10 * 1024 * 1024,   /* 10 MiB */
        .time_rotation_enabled = true,
        .rotation_interval_us  = 86400000000ULL,     /* one day */
        .max_rotated_files     = 14,
    };
    g_log = clog_create_from_file(path, CLOG_INFO, &rot, NULL);
    if (g_log == CLOG_INVALID) {
        remove_dir(dir);
        return 1;
    }
    clog_set_field(g_log, "app", "order-processor");

    process_order("ORD-1001", "Alice", "coffee mug");
    process_order("ORD-1002", "Bob", "desk lamp");
    clog_info(g_log, "batch done");

    clog_close(g_log);

    /* Show the contents of the file, then remove the files. */
    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f))
            fputs(line, stdout);
        fclose(f);
    }
    remove_dir(dir);
    return 0;
}
```

The file contains these lines (without the time and `proc`):

```
level=INFO src=orders.c:24 func=process_order customer=Alice order_id=ORD-1001 app=order-processor msg="order received: coffee mug"
level=INFO src=orders.c:29 func=process_order customer=Alice order_id=ORD-1001 app=order-processor msg="order shipped"
level=INFO src=orders.c:24 func=process_order customer=Bob order_id=ORD-1002 app=order-processor msg="order received: desk lamp"
level=WARN src=orders.c:27 func=process_order customer=Bob order_id=ORD-1002 app=order-processor msg="shipping failed with code 42"
level=INFO src=orders.c:74 func=main app=order-processor msg="batch done"
```

The order fields only show up on the lines of their own order, while the
`app` field of the root logger is on every line, because every derived
logger started with a copy of it.

## Example: worker threads with async JSON logging

Four worker threads each give themselves a name and get a derived logger
with their own `worker` field. All of them write through one async logger
that sends JSON to standard output, and the main thread waits for the
workers, writes a summary and flushes.

```c
/* workers.c: four named worker threads write JSON through one async logger. */
#include <ccollections/clogger.h>
#include <pthread.h>
#include <stdio.h>

#define NUM_WORKERS 4
#define JOBS_PER_WORKER 3

static clog g_log;

struct worker_arg {
    int id;
};

static void *worker(void *p) {
    struct worker_arg *arg = p;

    /* The name appears in the "proc" key of every record of this thread. */
    char name[16];
    snprintf(name, sizeof(name), "worker-%d", arg->id);
    ccol_set_thread_name(name);

    /* A private logger with its own fields; it shares the async writer. */
    clog wlog = clog_derive(g_log);
    if (wlog == CLOG_INVALID)
        return NULL;
    char id[16];
    snprintf(id, sizeof(id), "%d", arg->id);
    clog_set_field(wlog, "worker", id);

    for (int job = 0; job < JOBS_PER_WORKER; job++)
        clog_info(wlog, "finished job %d", job);

    clog_close(wlog);
    return NULL;
}

int main(void) {
    clog_async_cfg_t acfg = {
        .queue_size        = 1024,        /* a full queue blocks the caller */
        .flush_buffer_size = 64 * 1024,
        .flush_interval_us = 100000,      /* 100 ms */
    };
    g_log = clog_create_from_fd(1, CLOG_INFO, &acfg);
    if (g_log == CLOG_INVALID)
        return 1;
    clog_set_format(g_log, CLOG_FMT_JSON);

    pthread_t tids[NUM_WORKERS];
    struct worker_arg args[NUM_WORKERS];
    int started = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        args[i].id = i;
        if (pthread_create(&tids[i], NULL, worker, &args[i]) != 0)
            break;
        started++;
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);

    clog_info(g_log, "all %d workers done", started);
    clog_flush(g_log);   /* every record above is in stdout here */

    clog_close(g_log);
    return 0;
}
```

Build it with `-pthread`. Part of the output:

```
{"ts":"2026-10-06T16:25:27.078639Z","level":"INFO","proc":"workers(2742360):worker-0(2742362)","src":"workers.c:32","func":"worker","worker":"0","msg":"finished job 0"}
{"ts":"2026-10-06T16:25:27.078747Z","level":"INFO","proc":"workers(2742360):worker-1(2742363)","src":"workers.c:32","func":"worker","worker":"1","msg":"finished job 0"}
{"ts":"2026-10-06T16:25:27.079039Z","level":"INFO","proc":"workers(2742360):workers(2742360)","src":"workers.c:61","func":"main","msg":"all 4 workers done"}
```

Each worker closes its own derived handle while the other threads keep
writing through theirs, and that's safe. Closing a handle that another
thread is using is what you must not do.

## Example: watching rotation and compression

This program sets a tiny size limit so that rotation kicks in at once. It
writes 200 records and then lists the directory.

```c
/* rotate.c: show size rotation, compression and retention. */
#include <ccollections/clogger.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int by_name(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

static int not_dot(const struct dirent *e) {
    return e->d_name[0] != '.';
}

/* Print, and optionally delete, each file of a flat directory. */
static void walk_dir(const char *dir, int delete_them) {
    struct dirent **names;
    int n = scandir(dir, &names, not_dot, by_name);
    for (int i = 0; i < n; i++) {
        char path[4096];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]->d_name);
        if (delete_them)
            unlink(path);
        else if (stat(path, &st) == 0)
            printf("  %-40s %6lld bytes\n", names[i]->d_name,
                   (long long)st.st_size);
        free(names[i]);
    }
    if (n >= 0)
        free(names);
}

int main(void) {
    char dir[] = "/tmp/rotate-demo-XXXXXX";
    if (mkdtemp(dir) == NULL)
        return 1;
    char path[4096];
    snprintf(path, sizeof(path), "%s/app.log", dir);

    clog_rotation_cfg_t cfg = {
        .size_rotation_enabled = true,
        .max_file_size         = 4096,   /* tiny, so the demo rotates */
        .max_rotated_files     = 3,      /* keep three old generations */
        .compress_rotated      = true,   /* gzip each rotated file */
    };
    clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
    if (lg == CLOG_INVALID) {
        rmdir(dir);
        return 1;
    }

    for (int i = 0; i < 200; i++)
        clog_info(lg, "request %d served in %d ms", i, 3 + i % 17);

    /* The last close waits for every queued compression to finish. */
    clog_close(lg);

    printf("%s:\n", dir);
    walk_dir(dir, 0);
    walk_dir(dir, 1);
    rmdir(dir);
    return 0;
}
```

One run gave output like this:

```
/tmp/rotate-demo-uxpj7Y:
  app.log                                    2773 bytes
  app.log.20261006162537_0003.gz              356 bytes
  app.log.20261006162537_0004.gz              350 bytes
  app.log.20261006162537_0005.gz              352 bytes
```

All the rotations happened within one second, so their names carry the
suffixes `_0001`, `_0002` and so on. The oldest generations were deleted and
the three newest are compressed. `app.log` holds what was written since the
last rotation. A size rotation runs right after the write that reaches the
limit, so if the last record of a run happens to be that write, `app.log` is
empty (0 bytes). The sizes, and how many records end up in each file, depend
on the length of each record, which includes the program name and the time
stamp.

## Example: a command-line tool with -v and -q

This tool prints the size of every file you give it and sends its own
diagnostics to standard error through a logger. `-v` lowers the minimum
level to `DEBUG`, `-q` raises it to `ERROR`, and a usage error is fatal.

```c
/* lsize.c: print the size of each file; -v adds detail, -q shows less. */
#include <ccollections/clogger.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    clog lg = clog_create_from_fd(2, CLOG_INFO, NULL);
    if (lg == CLOG_INVALID)
        return 1;

    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (strcmp(argv[first], "-v") == 0)
            clog_set_level(lg, CLOG_DEBUG);
        else if (strcmp(argv[first], "-q") == 0)
            clog_set_level(lg, CLOG_ERROR);
        else
            clog_fatal(lg, "unknown option %s", argv[first]);  /* exits */
    }
    if (first == argc)
        clog_fatal(lg, "usage: %s [-v|-q] FILE...", argv[0]);  /* exits */

    long long total = 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        clog_debug(lg, "checking %s", argv[i]);
        if (stat(argv[i], &st) != 0) {
            clog_warn(lg, "skipping %s: %s", argv[i], strerror(errno));
            continue;
        }
        printf("%10lld %s\n", (long long)st.st_size, argv[i]);
        total += st.st_size;
    }
    clog_info(lg, "%d argument(s), %lld bytes in total", argc - first, total);

    clog_close(lg);
    return 0;
}
```

`./lsize -v lsize.c /nonexistent` prints the size of `lsize.c` on standard
output, and on standard error two `DEBUG` lines, a `WARN` line for the
missing file and the `INFO` summary. `./lsize` with no file writes one
`FATAL` record with a backtrace and exits with status 1.

## Good to know

A handle is a number, so compare it with `CLOG_INVALID`. Passing
`CLOG_INVALID` or a closed handle to a function stops the program with a
message, which means a use after close shows up immediately instead of
corrupting memory.

Don't close a handle that another thread is using. Every other call is
thread-safe, and if your threads start and stop independently you can give
each one its own derived logger, like the worker example does.

`clog_fatal` doesn't return, so code after it never runs.

Async records can get lost on `_exit`, `abort` or a crash. Call `clog_flush`
before a step that might go badly wrong.

Values outside an enum are refused. `clog_set_level`, `clog_set_format` and
`clog_set_facility` return `ccol_invalid_args` for a value that their enum
doesn't name (a level of `-3`, a format of `9`, a facility of `99`) and keep
the current setting, and `clog_create_from_file` and `clog_create_from_fd`
return `CLOG_INVALID` for such a minimum level.

A bad field key never shows up in the log, and nothing in the log says so;
`clog_set_field` tells you through its return value (`ccol_invalid_args`).
If a field doesn't show up, check that return value, and the key for spaces
or reserved names.

The logger never writes a malformed record. A record larger than 16 MiB, or
one it can't build because memory ran out, is replaced by a short record
that says so. When a write can't deliver records, a `log record truncated`
record reports them. On a full disk that record waits until there is room
again and goes out in front of the next record (or at the next flush of an
async logger, or at exit), so the log names the loss either way and no
record gets glued to the fragment of a cut one.

A full disk doesn't stop rotation. If the disk fills up in the middle of a
record while a rotation is due, the logger gives up the rest of that record
and rotates, which deletes the oldest generations and frees some room.
Every rotated generation ends with a newline: the logger closes a cut record
in the file it retires, or cuts that file back to its last whole line when
the file refuses even the newline.

## Reference

Overview:
[clogger(7)](../man/clogger/clogger.7)

Opening and closing:
[clog_create_from_fd_mp(3)](../man/clogger/clog_create_from_fd_mp.3)
(also `clog_create_from_fd`),
[clog_create_from_file_mp(3)](../man/clogger/clog_create_from_file_mp.3)
(also `clog_create_from_file`; rotation, compression, one file one logger),
[clog_derive(3)](../man/clogger/clog_derive.3),
[clog_flush(3)](../man/clogger/clog_flush.3),
[clog_close(3)](../man/clogger/clog_close.3)

Logging:
[clog_trace(3)](../man/clogger/clog_trace.3),
[clog_debug(3)](../man/clogger/clog_debug.3),
[clog_info(3)](../man/clogger/clog_info.3),
[clog_warn(3)](../man/clogger/clog_warn.3),
[clog_error(3)](../man/clogger/clog_error.3),
[clog_alert(3)](../man/clogger/clog_alert.3),
[clog_fatal(3)](../man/clogger/clog_fatal.3)

Levels, formats and fields:
[clog_set_level(3)](../man/clogger/clog_set_level.3),
[clog_get_level(3)](../man/clogger/clog_get_level.3),
[clog_set_format(3)](../man/clogger/clog_set_format.3),
[clog_get_format(3)](../man/clogger/clog_get_format.3),
[clog_set_facility(3)](../man/clogger/clog_set_facility.3),
[clog_get_facility(3)](../man/clogger/clog_get_facility.3),
[clog_set_field(3)](../man/clogger/clog_set_field.3),
[clog_remove_field(3)](../man/clogger/clog_remove_field.3),
[clog_clear_fields(3)](../man/clogger/clog_clear_fields.3)

Threads and signals:
[ccol_set_thread_name(3)](../man/clogger/ccol_set_thread_name.3),
[clog_set_signal_policy(3)](../man/clogger/clog_set_signal_policy.3)

Related guides:
[Concurrency](concurrency.md),
[Memory management](memory.md),
[Platforms](platforms.md),
[Thread pools](cthreadpool.md),
[HTTP server](chttpserver.md)
