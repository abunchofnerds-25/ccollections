# How the library works

This guide explains the ideas that all modules of c_collections share. Read
it one time. After that, the remainder of the library will be easy to learn.
You create, use and destroy a vector, a hash map, a thread pool and an HTTP
server in the same way.

To compile your first program, the first example below is enough. Read the
later sections when you must do one of these tasks:

- Give a container to another function.
- Handle a failure in your own code.
- Let the library do the cleanup automatically.

## Generic containers in C

C has no standard vector or map. Each usual alternative has a disadvantage:

- **`void *` everywhere.** One container works for all types, but the
  compiler cannot check the types. If you push a `double *` where an
  `int *` must go, the program compiles and then prints incorrect data.
- **Token pasting.** A macro makes a typed copy of the container for each
  element type. The compiler checks the types again. But you must make each
  combination manually, and it is difficult to debug the expanded code.
- **Code generators.** A separate tool writes the C code for you. This is
  easy to use, but it adds a step to the build.

c_collections uses the C11 `_Generic` keyword and three GNU extensions. GCC
and Clang both have these extensions: `__typeof__`, statement expressions and
`__attribute__((cleanup(...)))`. At compile time, a container macro examines
the static type of its arguments and selects the correct code path. There is
no generator and no hidden runtime. You write `cvec_push(v, 42)`, and the
compiler does the remainder of the work.

The cost is a compiler mode. You must compile code that *uses* the typed
macros with `-std=gnu11` or a later `gnu` mode. If a file only includes a
header, a strict `-std=c11` is enough. [Platforms](platforms.md) gives the
details.

## A first look

```c
#include <stdio.h>
#include <ccollections/cvector.h>
#include <ccollections/chashmap.h>

int main(void) {
    cvec_construct(scores, int);          /* a vector of int */
    chmap_construct(ages, char *, int);   /* a map from string to int */

    cvec_push(scores, 90);
    cvec_push(scores, 75);
    cvec_push(scores, 3.9);               /* the macro converts it to int, as in "int x = 3.9;" */

    chmap_insert(ages, "alice", 31);
    chmap_insert(ages, "bob", 27);

    for (size_t i = 0; i < cvec_size(scores); i++)
        printf("score[%zu] = %d\n", i, cvec_at(scores, i));
    printf("alice is %d\n", chmap_get(ages, "alice"));

    chmap_destroy(ages);                  /* frees the map and sets ages to NULL */
    cvec_destroy(scores);
    return 0;
}
```

Build it with an installed library (see [Building](building.md)):

```bash
gcc -std=gnu11 -o first first.c $(pkg-config --cflags --libs ccollections)
```

Look at these three points:

1. `cvec_construct(scores, int)` declares the variable `scores` and also
   creates the vector. You write the element type only one time, here.
2. `cvec_push(scores, 3.9)` stores `3`. The macro converts the value through
   the declared element type with a usual C assignment. It never copies the
   raw bytes of a `double` into an `int` slot.
3. A `char *` key is a string. The map copies the text into its own storage.
   Therefore, the buffer of the caller stays the property of the caller.

## Compile-time type dispatch

Each typed macro uses `_Generic` to select its code path from the declared
types. Some containers also keep a small tag of the key type and the value
type. Therefore, the decisions that the container makes at run time also follow
the declaration:

- `chashmap` selects its storage strategy from the key type and the value
  type.
- `cbstmap` selects signed, unsigned, floating-point or string order for its
  keys.
- `cvec_sort` selects the default comparator for the element type. If the
  type has no default comparator (for example, a struct), the compile fails.

The result for you is this: the compiler finds type errors, and one macro
name works for all element types.

## The lifecycle macros

Each container, and each module that gives you a handle, uses one name
pattern. The example below uses `cvec`:

| Macro | What it does |
|---|---|
| `cvec_declare(v, T)` | Declares the handle `v` (not initialized) and records the type `T` |
| `cvec_init(v)` | Creates the vector for a handle that you declared before |
| `cvec_construct(v, T)` | Does `declare` and `init` in one step |
| `cvec_construct_scoped(v, T)` | Does the same, and destroys `v` automatically at the end of the scope |
| `cvec_declare_scoped(v, T)` | Declares a scoped handle that you `init` later |
| `cvec_redeclare(v, T)` | Records the type again for a handle from a different scope |
| `cvec_destroy(v)` | Destroys the vector and sets `v` to NULL |

The typed containers `chmap_*`, `cbmap_*` and `clru_*` have the same set of
macros. They use a key type and a value type in place of `T`. Some modules
have a handle with no element type, for example `cstr_*`, `ctpool_*`,
`ccol_event_loop_*` and `chttpsvr_*`. These modules have the declare,
construct, scoped and destroy macros, but they do not need a redeclare
macro. Many modules also have an `_mp` form that takes a custom allocator.
See [Memory management](memory.md).

A destroy macro needs a modifiable variable, because the macro sets the
variable to NULL. If the handle is NULL, a destroy does nothing. Therefore, a
second destroy of the same variable causes no damage. Each destroy macro
evaluates each of its arguments exactly one time.

### Declare now, create later

If the program possibly does not need the container, do the two steps
separately:

```c
#include <stdio.h>
#include <ccollections/cvector.h>

int main(int argc, char **argv) {
    (void)argv;
    cvec_declare(names, char *);       /* declared, not created yet */
    if (argc > 0) {
        cvec_init(names);              /* allocate only when necessary */
        cvec_push(names, "first");
        printf("%zu name(s)\n", cvec_size(names));
        cvec_destroy(names);
    }
    return 0;
}
```

`cvec_declare` does not initialize the handle. Therefore, destroy the handle
only on a path that ran `cvec_init`. `cvec_declare_scoped` sets the handle to
NULL at the start. It destroys the handle at the end of the scope, if you
created the vector or not.

### Scoped cleanup

Manual cleanup often fails in a function that has many return paths. The
library destroys a `_scoped` handle on each path out of its scope:

```c
#include <stdio.h>
#include <ccollections/cvector.h>

/* Returns the sum of the even numbers in [0, n), or -1 when n is negative.
 * The library destroys the scoped vector on both return paths. */
static long sum_of_evens(int n) {
    cvec_construct_scoped(evens, int);
    if (n < 0)
        return -1;                     /* the library destroys evens here */
    for (int i = 0; i < n; i += 2)
        cvec_push(evens, i);
    long sum = 0;
    for (size_t i = 0; i < cvec_size(evens); i++)
        sum += cvec_at(evens, i);
    return sum;                        /* and here */
}

int main(void) {
    printf("%ld %ld\n", sum_of_evens(10), sum_of_evens(-1));
    return 0;
}
```

The scoped macros expand to declarations. Therefore, write each scoped macro
as a separate statement.

## Passing a container to another function

`cvec_construct(scores, int)` also creates a hidden local variable next to
`scores`. This variable records the element type. It exists only in the scope
where you wrote the macro. When you give `scores` to a different function,
only the handle goes to that function. The typed macros in the called
function do not compile until you give the type again with `*_redeclare`:

```c
#include <stdio.h>
#include <ccollections/chashmap.h>

/* The function receives the handle as a plain chmap. chmap_redeclare
 * gives the key type and the value type to this scope. Therefore, the
 * typed macros work here. */
static void count_word(chmap counts, const char *word) {
    chmap_redeclare(counts, char *, int);
    int *n = chmap_get_ptr(counts, word);
    if (n)
        (*n)++;
    else
        chmap_insert(counts, word, 1);
}

int main(void) {
    const char *text[] = { "to", "be", "or", "not", "to", "be" };
    chmap_construct(counts, char *, int);
    for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++)
        count_word(counts, text[i]);
    printf("to=%d be=%d or=%d\n", chmap_get(counts, "to"),
           chmap_get(counts, "be"), chmap_get(counts, "or"));
    chmap_destroy(counts);
    return 0;
}
```

The handle types are `cvec`, `chmap`, `cbmap`, `cstr` and some others. The
most frequent first error with this library is a missing `*_redeclare` in a
new scope. The compiler error gives the name of the missing companion
variable.

## Two layers: typed macros and raw functions

Each typed macro calls a raw function:

| Typed macro | Raw function |
|---|---|
| `chmap_insert(m, k, v)` | `chmap_insert_elem(m, &key_pair, &val_pair)` |
| `chmap_get(m, k)` | `chmap_get_elem_ref(m, &key_pair, &val_pair)` |
| `cvec_push(v, x)` | `cvector_push_back(v, &x)` |

The macros are the easy layer. They take values of the declared types, and
each failure stops the program. The raw functions take `cmap_pair` arguments
(a pointer and a size). They give a `ccol_retval_t` that you examine. Use
the raw layer when a failure is a result that you expect and that you want to
handle.

## Error handling

There are two types of error.

**Programming errors** are loud. Examples are a NULL where a handle is
necessary, an index out of range, a stale handle, and `chmap_get` of a key
that is not in the map. For these errors, the library writes the file, the
line and a message to stderr. Then it calls `abort()`. This does not change
with `NDEBUG`, because the checks are part of the contract. A typed macro
also stops the program when an allocation fails.

```c
#include <ccollections/chashmap.h>

int main(void) {
    chmap_construct(m, char *, int);
    int v = chmap_get(m, "missing");   /* a programming error: fatal */
    chmap_destroy(m);
    return v;
}
```

The program prints text similar to this:

```
Key dump (8 bytes):
  00000000  6d 69 73 73 69 6e 67 00                           |missing.|
prog.c:5: fatal: chmap_get('m'): r: -3 (ccol_key_not_found)
prog.c:5: ccol_assert failed: false
```

Then it stops with SIGABRT.

**Expected failures** are values. Examples are a key that is possibly not in
the map, a queue that is possibly empty, and a server that is possibly not
available. The library gives these failures as a `ccol_retval_t`. The value
is `0` (`ccol_success`) on success. Otherwise it is a negative code, for
example `ccol_key_not_found`, `ccol_timed_out` or
`ccol_http_connection_failed`. The values do not change in the full `1.x`
series. [ccollections(7)](../man/common/ccollections.7) lists all of them.
`ccol_retval_to_str` changes a value into its name, for use in a message:

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/chashmap.h>

/* Finds a port number by service name. Here a missing name is an
 * expected result. Therefore, the raw layer gives it as a value. */
static int port_of(chmap services, const char *name, int fallback) {
    cmap_pair key = { (void *)name, strlen(name) + 1 };
    int port;
    ccol_retval_t rv = chmap_get_elem_copy(services, &key, &port, sizeof(port));
    if (rv == ccol_key_not_found)
        return fallback;
    if (rv != ccol_success) {
        fprintf(stderr, "lookup failed: %s\n", ccol_retval_to_str(rv));
        return fallback;
    }
    return port;
}

int main(void) {
    chmap_construct(services, char *, int);
    chmap_insert(services, "http", 80);
    chmap_insert(services, "https", 443);

    printf("https -> %d\n", port_of(services, "https", -1));
    printf("gopher -> %d\n", port_of(services, "gopher", -1));

    /* The typed layer also has a non-fatal lookup: chmap_get_ptr gives NULL. */
    int *p = chmap_get_ptr(services, "ssh");
    printf("ssh is %s\n", p ? "known" : "unknown");

    chmap_destroy(services);
    return 0;
}
```

In a `cmap_pair`, the size of a string key includes the terminating NUL.
That is why the example gives `strlen(name) + 1`.

Many typed macros also have a related macro that does not stop the program.
An example is `chmap_get_ptr` above, which gives NULL for a missing key.
Read the module guide before you use the raw layer.

## Scoped raw pointers

The `_scoped` container macros apply only to the types of the library. For a
usual heap block, `common.h` gives the same automatic cleanup:

| Macro | What it does |
|---|---|
| `ccol_scoped_ptr(name, T)` | Declares `T *name = NULL`, and frees it with `free()` at the end of the scope |
| `ccol_scoped_ptr_mp(name, T, procs)` | Does the same, but frees with `procs->free` |
| `ccol_scoped_ptr_release(name)` | Returns the pointer and sets `name` to NULL, which cancels the free |

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ccollections/common.h>

/* Reads a full file into a NUL-terminated buffer. Each early return frees
 * the buffer automatically. The successful return gives it to the caller. */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    ccol_scoped_ptr(buf, char);
    size_t cap = 4096, len = 0;
    buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 == cap) {
            char *bigger = realloc(buf, cap * 2);
            if (!bigger) { fclose(f); return NULL; }   /* frees buf */
            buf = bigger;
            cap *= 2;
        }
    }
    fclose(f);
    buf[len] = '\0';
    return ccol_scoped_ptr_release(buf);   /* the caller is the owner after this */
}

int main(int argc, char **argv) {
    char *text = slurp(argc > 1 ? argv[1] : "/etc/hostname");
    if (!text) { perror("slurp"); return 1; }
    printf("%zu bytes\n", strlen(text));
    free(text);
    return 0;
}
```

Two limits apply. They apply to all cleanup-attribute guards in C:

- The guard frees only the **last** value of the pointer. If you assign a
  new block to the pointer, free the old block first. The `realloc` above is
  correct, because `realloc` itself releases the old block.
- To give the block to a different owner, call `ccol_scoped_ptr_release`.
  If you do not, the guard frees the block while the new owner uses it.

## Timeouts and durations

Each timeout, interval and duration in the API is a `uint64_t` number of
**microseconds**. Its name ends in `_us`. Examples are `timeout_us`,
`flush_interval_us` and the `_us` fields of the HTTP server configuration.
No call takes a `struct timespec` or a `time_t`.

```c
#include <stdio.h>
#include <ccollections/cthreadcomm.h>

int main(void) {
    ccol_circular_queue *q = ccol_circular_queue_create(8, NULL);
    if (!q) return 1;
    c_message_t msg;

    /* 0 microseconds: do not wait. Do the same as the try_ call. */
    ccol_retval_t rv = ccol_circq_timed_recv_zc(q, &msg, 0);
    printf("timeout 0      -> %s\n", ccol_retval_to_str(rv));

    /* 50 ms, written in microseconds. */
    rv = ccol_circq_timed_recv_zc(q, &msg, 50 * 1000);
    printf("timeout 50 ms  -> %s\n", ccol_retval_to_str(rv));

    ccol_circular_queue_destroy(q);
    return 0;
}
```

```
timeout 0      -> ccol_container_empty
timeout 50 ms  -> ccol_timed_out
```

Remember these points:

- Waits use the monotonic clock. Therefore, a change to the system time does not
  make a wait shorter or longer.
- A very large value, for example `UINT64_MAX`, is safe. It means "as long
  as the clock can count".
- For the timed queue calls and the timed thread-pool calls, `0` means "do
  not wait". You get the result of the `try_` call, not `ccol_timed_out`.
  For other fields, the man page of the field tells what `0` means. Usually
  it means "no limit" or "the default".

## Putting it together: a small grade book

This program keeps records in a vector of structs. It finds a record by name
through a map. It gives the two containers to helper functions. It uses a
scoped vector for a temporary sorted list.

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/chashmap.h>
#include <ccollections/cvector.h>

struct student {
    char name[32];
    double total;
    int count;
};

/* Records one grade. The roster holds the records. The index maps a name
 * to its position in the roster. The function receives both as plain
 * handles. */
static void record(cvec roster, chmap index, const char *name, double grade) {
    cvec_redeclare(roster, struct student);
    chmap_redeclare(index, char *, size_t);

    size_t *pos = chmap_get_ptr(index, name);
    if (!pos) {
        struct student s = { .total = 0, .count = 0 };
        snprintf(s.name, sizeof(s.name), "%s", name);
        cvec_push(roster, s);
        chmap_insert(index, name, cvec_size(roster) - 1);
        pos = chmap_get_ptr(index, name);
    }
    struct student *st = cvec_at_ptr(roster, *pos);
    st->total += grade;
    st->count++;
}

/* Gives the median of all averages. It sorts a scoped vector with the
 * default comparator for double. The library destroys the vector at the
 * return. */
static double median_average(cvec roster) {
    cvec_redeclare(roster, struct student);
    cvec_construct_scoped(avgs, double);
    for (size_t i = 0; i < cvec_size(roster); i++) {
        struct student s = cvec_at(roster, i);
        cvec_push(avgs, s.total / s.count);
    }
    cvec_sort(avgs);
    size_t n = cvec_size(avgs);
    if (n == 0) return 0.0;
    return n % 2 ? cvec_at(avgs, n / 2)
                 : (cvec_at(avgs, n / 2 - 1) + cvec_at(avgs, n / 2)) / 2;
}

int main(void) {
    static const struct { const char *name; double grade; } input[] = {
        { "ada", 91 }, { "linus", 78 }, { "ada", 87 },
        { "grace", 95 }, { "linus", 84 }, { "grace", 99 },
    };
    cvec_construct(roster, struct student);
    chmap_construct(index, char *, size_t);

    for (size_t i = 0; i < sizeof(input) / sizeof(input[0]); i++)
        record(roster, index, input[i].name, input[i].grade);

    for (size_t i = 0; i < cvec_size(roster); i++) {
        struct student *s = cvec_at_ptr(roster, i);
        printf("%-6s %5.1f\n", s->name, s->total / s->count);
    }
    printf("median %5.1f\n", median_average(roster));

    chmap_destroy(index);
    cvec_destroy(roster);
    return 0;
}
```

The pointer from `cvec_at_ptr` is valid only until the size of the vector
changes. Therefore, the program gets the pointer after the push, and never
before it.

## Good to know

- **Compile with `-std=gnu11`** (or a later `gnu` mode) each file that uses
  the typed macros.
- **Redeclare in each new scope** that uses typed macros on a handle that it
  received.
- **A destroy sets the handle to NULL.** The destroy does not clear copies
  of the handle in other variables. Do not use these copies after the
  destroy.
- **Typed macros stop the program on failure.** Use the related non-fatal
  macro or the raw function when a failure is usual for your program.
- **Times are in microseconds.** `1000` is one millisecond, not one second.

## Reference

Rules for the full library:
[ccollections(7)](../man/common/ccollections.7),
[ccol_retval_to_str(3)](../man/common/ccol_retval_to_str.3),
[ccol_scoped_ptr(3)](../man/common/ccol_scoped_ptr.3),
[ccol_scoped_ptr_mp(3)](../man/common/ccol_scoped_ptr_mp.3),
[ccol_scoped_ptr_release(3)](../man/common/ccol_scoped_ptr_release.3)

Lifecycle macros, with the vector as the example:
[cvec_declare(3)](../man/cvector/cvec_declare.3),
[cvec_init(3)](../man/cvector/cvec_init.3),
[cvec_construct(3)](../man/cvector/cvec_construct.3),
[cvec_construct_scoped(3)](../man/cvector/cvec_construct_scoped.3),
[cvec_declare_scoped(3)](../man/cvector/cvec_declare_scoped.3),
[cvec_redeclare(3)](../man/cvector/cvec_redeclare.3),
[cvec_destroy(3)](../man/cvector/cvec_destroy.3)

Typed layer and raw layer of the map:
[chmap_insert(3)](../man/chashmap/chmap_insert.3),
[chmap_insert_elem(3)](../man/chashmap/chmap_insert_elem.3),
[chmap_get(3)](../man/chashmap/chmap_get.3),
[chmap_get_ptr(3)](../man/chashmap/chmap_get_ptr.3),
[chmap_get_elem_ref(3)](../man/chashmap/chmap_get_elem_ref.3),
[chmap_get_elem_copy(3)](../man/chashmap/chmap_get_elem_copy.3)

Related guides:
[Building and linking](building.md),
[Platforms](platforms.md),
[Memory management](memory.md),
[Concurrency](concurrency.md),
[cvector](cvector.md),
[chashmap](chashmap.md)
