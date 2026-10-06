# clogger: structured logging

`clogger` writes log records that a program can search, filter and collect.
A record is not free-form text. It is a sequence of `key=value` pairs. The
pairs give the time, the level, the process and the thread, the source
location, the fields that you attached, and the message.

```
ts=2026-05-29T21:52:39.096473Z level=INFO proc=myapp(1234):main(1234) src=main.c:9 func=main env=prod msg="starting up"
```

Use `clogger` when:

- you want logs that tools such as `grep`, `jq` or a log collector can read
  without guesses,
- several threads write log records at the same time, and their lines must
  not mix,
- you want rotation of log files, or logs that go to the syslog daemon of
  the system, without your own code for these functions,
- you want an error record to have a backtrace.

If a short single-threaded tool prints only a small number of messages,
`fprintf(stderr, ...)` is simpler, and it is a correct choice.

```c
#include <ccollections/clogger.h>
```

## A first logger

```c
#include <ccollections/clogger.h>

int main(void) {
    /* A logger that writes to stderr (fd 2), synchronously. */
    clog lg = clog_open_fd(2, CLOG_INFO, NULL);
    if (lg == CLOG_INVALID)
        return 1;

    /* Fields that each later record of this logger contains. */
    clog_set_field(lg, "service", "billing");
    clog_set_field(lg, "env", "dev");

    ccol_log_info(lg, "starting up");
    ccol_log_debug(lg, "you will not see this: DEBUG is below INFO");
    ccol_log_warn(lg, "config key %s missing, using %d", "timeout", 30);
    ccol_log_error(lg, "payment gateway returned %d", 503);

    clog_close(lg);
    return 0;
}
```

Build it with `-rdynamic`. Then the backtraces show function names:

```sh
gcc -std=gnu11 -rdynamic first.c -lccollections -o first
```

The output is similar to this (the order of the fields can be different):

```
ts=2026-10-06T16:24:43.551077Z level=INFO proc=first(2741242):first(2741242) src=first.c:13 func=main env=dev service=billing msg="starting up"
ts=2026-10-06T16:24:43.551121Z level=WARN proc=first(2741242):first(2741242) src=first.c:15 func=main env=dev service=billing msg="config key timeout missing, using 30"
ts=2026-10-06T16:24:43.551276Z level=ERROR proc=first(2741242):first(2741242) src=first.c:16 func=main env=dev service=billing msg="payment gateway returned 503"
	#0 ./first(main+0x186) [0x55b9358482ef]
	#1 /lib/x86_64-linux-gnu/libc.so.6(+0x29ca8) [0x7fa19bfe7ca8]
	...
```

Important points:

- `clog` is a handle. It is a plain number, not a pointer. `CLOG_INVALID`
  (zero) means "no logger". Do not cast a `clog` to `void *` or from
  `void *`.
- The `ccol_log_*` macros take a `printf` format. They record the file, the
  line and the function for you.
- `ccol_log_error` adds a backtrace as lines that start with a tab. These
  lines do not start with `ts=`. Therefore, a log collector can attach them to
  the record above them.
- `clog_close` frees the logger. It does not close fd 2. A logger does not
  close a descriptor that you gave it.

## Reading a record

Each record starts with the same keys:

| Key | Meaning |
|---|---|
| `ts` | UTC time in ISO 8601, with microseconds |
| `level` | `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `ALERT` or `FATAL` |
| `proc` | `name(pid):thread(tid)`: the program name and the process ID, then the thread name and the thread ID |
| `src` | `file:line` of the log call |
| `func` | the function that wrote the record |

Your fields come next, and then `msg`. The logger writes some values in
double quotes, with backslash escapes. These are values that contain a
space, `=`, `"`, a backslash or a control character. Therefore, a message that
contains a newline also makes exactly one line:

```
msg="two\nlines \"quoted\" back\\slash"
```

## Levels

From the most detailed level to the most severe level:

| Level | Use it for |
|---|---|
| `CLOG_TRACE` | detail for each step, while you look for the cause of a problem |
| `CLOG_DEBUG` | detail that a developer wants and an operator does not want |
| `CLOG_INFO` | usual events; the usual minimum in production |
| `CLOG_WARN` | an unusual condition that the program managed |
| `CLOG_ERROR` | a failure; adds a backtrace |
| `CLOG_ALERT` | an operator must act immediately; adds a backtrace |
| `CLOG_FATAL` | the program cannot continue; adds a backtrace, then exits |

Each level has a macro: `ccol_log_trace`, `ccol_log_debug`, `ccol_log_info`,
`ccol_log_warn`, `ccol_log_error`, `ccol_log_alert` and `ccol_log_fatal`.

You select the minimum level when you open the logger. You can change it at
all times with `clog_set_level`. A record below the minimum costs almost
nothing, and the logger discards it. With `CLOG_OFF` as the minimum, the
logger writes no records except `CLOG_FATAL` records.

`ccol_log_fatal` is special. It always writes, whatever the minimum. Then it
stops the process with `exit(EXIT_FAILURE)`. It does not return. Before it
exits, it makes sure that the records that the program wrote before it also
go into the log. Therefore, the last records of a program that stops are not
lost.

## Fields

A field is a `key=value` pair. After you set it, the logger adds it to each
record. Use fields for context that stays the same for many records: for
example, a service name, a request ID, or a user.

```c
#include <ccollections/clogger.h>

int main(void) {
    clog lg = clog_open_fd(1, CLOG_INFO, NULL);
    if (lg == CLOG_INVALID)
        return 1;

    clog_set_field(lg, "request_id", "abc-123");
    ccol_log_info(lg, "processing");

    clog_set_field(lg, "request_id", "abc-124");   /* replaces the value */
    clog_set_field(lg, "path", "/tmp/my file");    /* in quotes in the output */
    ccol_log_info(lg, "processing");

    clog_remove_field(lg, "request_id");
    ccol_log_info(lg, "between requests");

    clog_clear_fields(lg);
    ccol_log_info(lg, "no fields at all");

    clog_close(lg);
    return 0;
}
```

Without the `ts` and `proc` keys, the output is:

```
level=INFO src=fields.c:9 func=main request_id=abc-123 msg=processing
level=INFO src=fields.c:13 func=main path="/tmp/my file" request_id=abc-124 msg=processing
level=INFO src=fields.c:16 func=main path="/tmp/my file" msg="between requests"
level=INFO src=fields.c:19 func=main msg="no fields at all"
```

The logger copies the key and the value. Therefore, you can free your strings or
use them again immediately.

Keys must be plain printable ASCII. They must not contain spaces, `=`, `]`,
`"` or `\`. A key must not have one of the fixed names (`ts`, `level`,
`proc`, `src`, `func`, `msg`, `bt`, `bt_error`). `clog_set_field` ignores a
bad key and does not report it. Therefore, use simple keys, for example
`request_id` or `user`.

## Derived loggers: one logger per task

Frequently you want more fields for one item of work, for example one HTTP
request or one order. You do not want to change the fields of the remaining
program. `clog_derive` gives you a second handle on the same output:

```c
clog req_log = clog_derive(app_log);      /* fragment */
if (req_log == CLOG_INVALID)
    return;                               /* no memory */
clog_set_field(req_log, "request_id", id);
ccol_log_info(req_log, "handling %s", path);
clog_close(req_log);                      /* app_log stays open */
```

A derived logger starts with a copy of the fields and the level of its
parent. After that, the two loggers are independent. A field that you set on
one logger does not show on the other logger. They share all properties of
the output: the file or the descriptor, the format, the rotation, and the
lock that prevents lines from mixing. The output stays open until you close
the root and all derived handles. See the example for orders below.

## Output formats

`clog_set_format` changes the format of a logger at any time. The change
also applies to each handle that shares its output.

**logfmt** (`CLOG_FMT_LOGFMT`, the default) is the format of the examples
above. A person can read it, and a program can parse it easily.

**JSON** (`CLOG_FMT_JSON`) writes one JSON object on each line (NDJSON). Your
fields become top-level keys. A backtrace becomes a `"bt"` array in the same
object:

```
{"ts":"2026-10-06T16:24:58.272421Z","level":"INFO","proc":"app(2741544):app(2741544)","src":"app.c:10","func":"main","env":"prod","msg":"starting up"}
{"ts":"2026-10-06T16:24:58.272599Z","level":"ERROR","proc":"app(2741544):app(2741544)","src":"app.c:11","func":"main","env":"prod","msg":"db failed: timeout","bt":["#0 ./app(main+0xf5) [0x56479704526e]","#1 ..."]}
```

Each string in a JSON record is valid UTF-8, also when you log binary data.
The logger replaces each invalid byte with U+FFFD. Therefore, you can safely
give the output to `jq` or to a JSON collector.

**Syslog** (`CLOG_FMT_SYSLOG`) writes RFC 5424 messages for the system log
daemon. It works only on a logger that you opened with `clog_open_fd`. On a
file logger, the call does nothing. On Linux, connect a datagram socket to
`/dev/log`:

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
        lg = clog_open_fd(fd, CLOG_INFO, NULL);
    if (lg == CLOG_INVALID) {
        close(fd);
        return 1;
    }
    clog_set_format(lg, CLOG_FMT_SYSLOG);
    clog_set_facility(lg, CLOG_SYSLOG_DAEMON);

    ccol_log_info(lg, "service started");

    clog_close(lg);   /* does not close fd */
    close(fd);
    return 0;
}
```

The daemon receives:

```
<30>1 2026-10-06T16:24:58.377293Z myhost myservice 2741559 INFO [ccol proc="myservice(2741559):myservice(2741559)" src="svc.c:26" func="main"] service started
```

Keep syslog messages below approximately 2 KiB. A usual syslog daemon
accepts this size in one datagram.

## Logging to a file, with rotation

`clog_open_file` opens (or creates) a file and appends to it. Give a
`clog_rotation_cfg_t` to rotate the file. Give `NULL` for a plain file that
only becomes larger:

```c
clog_rotation_cfg_t cfg = {                          /* fragment */
    .size_rotation_enabled = true,
    .max_file_size         = 50 * 1024 * 1024,      /* 50 MiB */
    .time_rotation_enabled = true,
    .rotation_interval_us  = 86400000000ULL,        /* one day */
    .max_rotated_files     = 7,                     /* keep one week */
    .compress_rotated      = true,                  /* gzip old files */
};
clog lg = clog_open_file("/var/log/myapp/app.log", CLOG_INFO, &cfg, NULL);
```

The effects on the disk:

- A rotation renames `app.log` to `app.log.<YYYYMMDDHHMMSS>` and starts a
  new `app.log`. A second rotation in the same second gets `_0001`, a third
  rotation gets `_0002`, and so on.
- The logger keeps the newest `max_rotated_files` old files and deletes the
  other old files. When you set a value to zero, the logger uses a default
  value (10 MiB, one day, 7 files). You cannot disable the deletion.
  Therefore, old logs do not fill the disk.
- With `compress_rotated`, each old file becomes a `.gz` file that `zcat`
  can read. A background thread of the logger does the compression.
  Therefore, a log call does not wait for gzip. If you link the static library,
  also link zlib (`-lz`).
- A new `app.log` gets the permissions and the owner of the file that it
  replaces. Therefore, a log that you restricted to `0600` stays restricted.
- The logger works in the directory where it opened the file. Therefore, a
  relative path continues to work after your program calls `chdir`.
- The logger renames, compresses and deletes only the files that it
  identifies as its own rotated files. It does not change other files in the
  directory.

The rotation example below shows all of this on a real directory.

**One file, one logger.** Two parts of your program can call
`clog_open_file` on the same rotating file. Then the second call joins the
first logger, as `clog_derive` does. Therefore, the two parts do not both
rename the file. The second call fails with `CLOG_INVALID` if its rotation,
async or allocator settings are different from the first. For the details,
see [clog_open_file_mp(3)](../man/clogger/clog_open_file_mp.3).

## Async logging

By default, each log call formats and writes its record before it returns.
This is simple and safe. But a slow disk or a busy pipe then makes the
thread that writes the log slower.

Give a `clog_async_cfg_t` to put a logger into async mode. The thread that
writes the log then only formats the message. It gives the record to a
writer thread of the logger. The writer thread collects records and writes
them in batches:

```c
clog_async_cfg_t acfg = {                     /* fragment */
    .queue_size        = 4096,                /* 0 = unbounded */
    .flush_buffer_size = 64 * 1024,           /* write when 64 KiB collect */
    .flush_interval_us = 200000,              /* ... or each 200 ms */
};
clog lg = clog_open_file("app.log", CLOG_INFO, NULL, &acfg);
```

- `queue_size` sets the backpressure. Zero means an unbounded queue, which
  does not block a caller. With a positive size, a full queue makes the
  caller wait. The logger does not discard a record.
- A pointer that is not NULL puts the logger into async mode. This is also
  true for a pointer to a struct of zeros, which selects the defaults
  (64 KiB and 200 ms). Only `NULL` means synchronous.
- Each logger that you derive from an async logger is also async.
- `clog_flush` waits until the logger gives all records that you logged
  before the call to the operating system. Call it before a step that can
  cause a crash.

The effects at exit:

- `clog_close`, a normal `exit()`, and a return from `main` write the records
  that are in the queue. The exit path waits a maximum of 5 seconds.
- `ccol_log_fatal` first writes the records that earlier calls put into the
  queue. Then it writes its own record, and then it exits.
- `_exit()`, `abort()` and a crash on a signal run no exit handlers.
  Therefore, the records in the queue are lost. Where this is important, call
  `clog_flush` first.

## Thread names

The `proc` key gives the name of the thread that wrote the record. By
default, this is the name that the operating system gives the thread. For a
new thread, this is usually the program name. Give your threads names that
have a meaning:

```c
static void *worker(void *arg) {                     /* fragment */
    ccol_set_thread_name("ingest-worker");
    ccol_log_info(lg, "started");   /* proc=myapp(1234):ingest-worker(1240) */
    return NULL;
}
```

`ccol_set_thread_name` keeps the first 15 bytes of the name (the Linux
limit). It does not need a logger, and it renames only the thread that
calls it. To rename a thread that writes log records, use this function, not
`pthread_setname_np`. The logger reads the name of a thread one time, at the
first record of that thread. After that, it does not see a rename that a
different function makes.

## Backtraces

`ccol_log_error`, `ccol_log_alert` and `ccol_log_fatal` add a backtrace.
Link with `-rdynamic` to see function names in it. The capture works on
glibc, FreeBSD and macOS. When the logger cannot get a backtrace, the record
tells you (`#error backtrace unavailable`, or a `"bt_error"` key in JSON).
The logger does not silently omit the backtrace.

## Pipes, sockets and SIGPIPE

A logger can write to all types of descriptor: a pipe into a different
program, a socket to a log collector, or a terminal. Usually, a write to a
pipe that has no reader causes `SIGPIPE`, which stops the process. By
default, `clogger` prevents this. The logger discards a write to a broken
pipe or socket. If your application must manage `SIGPIPE` itself, call
`clog_set_sigpipe_policy(CLOG_SIGPIPE_UNTOUCHED)` before the first log
write. [clog_set_sigpipe_policy(3)](../man/clogger/clog_set_sigpipe_policy.3)
describes the two policies.

## Custom allocators and fork

`clog_open_fd_mp` and `clog_open_file_mp` take a `ccol_memmgmt_procs_t` as
their last argument. Then the logger allocates through your functions. Set
all four function pointers. Derived loggers use the same allocator. See
[Memory management](memory.md).

If your program calls `fork()`, fork before you open a logger. As an
alternative, open the logger and then call `exec` in the child immediately.
For the policy behind that rule, see [Concurrency](concurrency.md).

## Example: per-order logging in a rotating file

A small shop processes customer orders. Each record of an order contains the
order ID and the customer. No other record of the program contains them. The
log is in a temporary directory, and the program removes it at the end.

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

    ccol_log_info(order_log, "order received: %s", item);
    int rc = ship_item(item);
    if (rc != 0)
        ccol_log_warn(order_log, "shipping failed with code %d", rc);
    else
        ccol_log_info(order_log, "order shipped");

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
    g_log = clog_open_file(path, CLOG_INFO, &rot, NULL);
    if (g_log == CLOG_INVALID) {
        remove_dir(dir);
        return 1;
    }
    clog_set_field(g_log, "app", "order-processor");

    process_order("ORD-1001", "Alice", "coffee mug");
    process_order("ORD-1002", "Bob", "desk lamp");
    ccol_log_info(g_log, "batch done");

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

The order fields show only on the lines of their own order. The `app` field
of the root logger shows on all lines, because each derived logger started
with a copy of it.

## Example: worker threads with async JSON logging

Each of four worker threads gives itself a name. Each worker gets a derived
logger with its own `worker` field. All workers write through one async
logger that writes JSON to standard output. The main thread waits for the
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

    /* The name shows in the "proc" key of each record of this thread. */
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
        ccol_log_info(wlog, "finished job %d", job);

    clog_close(wlog);
    return NULL;
}

int main(void) {
    clog_async_cfg_t acfg = {
        .queue_size        = 1024,        /* a full queue blocks the caller */
        .flush_buffer_size = 64 * 1024,
        .flush_interval_us = 100000,      /* 100 ms */
    };
    g_log = clog_open_fd(1, CLOG_INFO, &acfg);
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

    ccol_log_info(g_log, "all %d workers done", started);
    clog_flush(g_log);   /* all records above are in stdout at this point */

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

Each worker closes only its own derived handle, while other threads write
through their handles. That is safe. It is not safe to close a handle that a
different thread uses.

## Example: watching rotation and compression

This program sets a very small size limit. Therefore, rotation occurs
immediately. The program writes 200 records and then lists the directory.

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
        .max_file_size         = 4096,   /* very small: the demo rotates */
        .max_rotated_files     = 3,      /* keep three old generations */
        .compress_rotated      = true,   /* gzip each rotated file */
    };
    clog lg = clog_open_file(path, CLOG_INFO, &cfg, NULL);
    if (lg == CLOG_INVALID) {
        rmdir(dir);
        return 1;
    }

    for (int i = 0; i < 200; i++)
        ccol_log_info(lg, "request %d served in %d ms", i, 3 + i % 17);

    /* The last close waits until all queued compressions are complete. */
    clog_close(lg);

    printf("%s:\n", dir);
    walk_dir(dir, 0);
    walk_dir(dir, 1);
    rmdir(dir);
    return 0;
}
```

The output of one run is similar to this:

```
/tmp/rotate-demo-uxpj7Y:
  app.log                                    2773 bytes
  app.log.20261006162537_0003.gz              356 bytes
  app.log.20261006162537_0004.gz              350 bytes
  app.log.20261006162537_0005.gz              352 bytes
```

All the rotations occurred in one second. Therefore, their names have the
suffix `_0001`, `_0002`, and so on. The logger deleted the oldest
generations. The three newest generations are compressed, and `app.log`
contains the latest records.

## Example: a command-line tool with -v and -q

This tool prints the size of each file that you give it. Its own
diagnostics go to standard error through a logger:

- `-v` decreases the minimum level to `DEBUG`;
- `-q` increases the minimum level to `ERROR`;
- a usage error is fatal.

```c
/* lsize.c: print the size of each file; -v adds detail, -q shows less. */
#include <ccollections/clogger.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    clog lg = clog_open_fd(2, CLOG_INFO, NULL);
    if (lg == CLOG_INVALID)
        return 1;

    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (strcmp(argv[first], "-v") == 0)
            clog_set_level(lg, CLOG_DEBUG);
        else if (strcmp(argv[first], "-q") == 0)
            clog_set_level(lg, CLOG_ERROR);
        else
            ccol_log_fatal(lg, "unknown option %s", argv[first]);  /* exits */
    }
    if (first == argc)
        ccol_log_fatal(lg, "usage: %s [-v|-q] FILE...", argv[0]);  /* exits */

    long long total = 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        ccol_log_debug(lg, "checking %s", argv[i]);
        if (stat(argv[i], &st) != 0) {
            ccol_log_warn(lg, "skipping %s: %s", argv[i], strerror(errno));
            continue;
        }
        printf("%10lld %s\n", (long long)st.st_size, argv[i]);
        total += st.st_size;
    }
    ccol_log_info(lg, "%d argument(s), %lld bytes in total", argc - first, total);

    clog_close(lg);
    return 0;
}
```

`./lsize -v lsize.c /nonexistent` prints the size of `lsize.c` on standard
output. On standard error, it prints two `DEBUG` lines, a `WARN` line for
the missing file, and the `INFO` summary. `./lsize` without a file writes
one `FATAL` record with a backtrace and exits with status 1.

## Good to know

- **A handle is a number.** Compare it with `CLOG_INVALID`. If you give
  `CLOG_INVALID` or a closed handle to a function, the program stops with a
  message. Therefore, you see a use after close immediately, and memory does not
  become corrupt.
- **Do not close a handle that a different thread uses.** All other calls
  are thread-safe. If threads start and stop independently, give each thread
  its own derived logger, as in the worker example.
- **`ccol_log_fatal` does not return.** Code after it does not run.
- **Async records can be lost on `_exit`, `abort` or a crash.** Call
  `clog_flush` before a step that can fail badly.
- **The syslog format needs a descriptor logger.** `clog_set_format` with
  `CLOG_FMT_SYSLOG` on a file logger does nothing.
- **The logger ignores bad field keys and does not report them.** If a
  field does not show, examine the key for spaces or reserved names.
- **The logger never writes a malformed record.** It replaces a record that
  is larger than 16 MiB, or a record that it cannot make because no memory
  is available, with a short record that tells you this. When a write cannot
  deliver records, a `log record truncated` record reports them.

## Reference

Overview:
[clogger(7)](../man/clogger/clogger.7)

Opening and closing:
[clog_open_fd_mp(3)](../man/clogger/clog_open_fd_mp.3)
(also `clog_open_fd`),
[clog_open_file_mp(3)](../man/clogger/clog_open_file_mp.3)
(also `clog_open_file`; rotation, compression, one file one logger),
[clog_derive(3)](../man/clogger/clog_derive.3),
[clog_flush(3)](../man/clogger/clog_flush.3),
[clog_close(3)](../man/clogger/clog_close.3)

Logging:
[ccol_log_trace(3)](../man/clogger/ccol_log_trace.3),
[ccol_log_debug(3)](../man/clogger/ccol_log_debug.3),
[ccol_log_info(3)](../man/clogger/ccol_log_info.3),
[ccol_log_warn(3)](../man/clogger/ccol_log_warn.3),
[ccol_log_error(3)](../man/clogger/ccol_log_error.3),
[ccol_log_alert(3)](../man/clogger/ccol_log_alert.3),
[ccol_log_fatal(3)](../man/clogger/ccol_log_fatal.3)

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
[clog_set_sigpipe_policy(3)](../man/clogger/clog_set_sigpipe_policy.3)

Related guides:
[Concurrency](concurrency.md),
[Memory management](memory.md),
[Platforms](platforms.md),
[Thread pools](cthreadpool.md),
[HTTP server](chttpserver.md)
