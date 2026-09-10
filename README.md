# C Collections

`c_collections` is a library of generic data structures and utilities for C. It provides what the C standard library leaves out: dynamic arrays, hash maps, ordered maps, dynamic strings, memory pools, inter-thread communication primitives, a thread pool, a structured logger, a JSON parser, a YAML parser, an HTTP client, and an HTTP server; all under one consistent API.

If you have used C++'s `vector` and `map`, Java's `ArrayList` and `HashMap`, or Python's `list` and `dict`, the containers here will feel familiar. The difference is that this library is C11 (plus the GNU C extensions GCC and Clang both support: `typeof`, statement expressions, and `__attribute__((cleanup(...)))`); no code generators, no external build tools, no hidden runtime.

Every module follows the same naming conventions (`*_construct`, `*_destroy`, and optional `*_scoped` variants for automatic cleanup), so once you have learned how one container works, the others follow naturally. The library compiles cleanly under GCC and Clang at `-Wall -Wextra -Werror`, and each module ships with a test suite that runs under Valgrind.

Every public function and macro also has a real troff manual page under [`man/`](man/) (`man/<module>/`, one file per symbol); see [`man/README`](man/README) for how to browse them with `man -l`.

---

## Table of Contents

1. [Rationale](#1-rationale)
2. [How Generic Containers Work in C](#2-how-generic-containers-work-in-c)
3. [Design Principles](#3-design-principles)
   - [Compile-Time Type Dispatch](#31-compile-time-type-dispatch)
   - [Container Lifecycle Macros](#32-container-lifecycle-macros)
   - [Cross-Scope Type Recovery](#33-cross-scope-type-recovery)
   - [Error Handling](#34-error-handling)
   - [Scoped Raw Pointers](#35-scoped-raw-pointers)
4. [Building and Linking](#4-building-and-linking)
5. [Dynamic Array - `cvector`](#5-dynamic-array--cvector)
6. [Dynamic String - `cstring`](#6-dynamic-string--cstring)
7. [Hash Map - `chashmap`](#7-hash-map--chashmap)
8. [Ordered Map - `cbstmap`](#8-ordered-map--cbstmap)
9. [Unified Iteration - `citerators`](#9-unified-iteration--citerators)
10. [Sorting - `csort`](#10-sorting--csort)
11. [Memory Pools - `cmempool`](#11-memory-pools--cmempool)
12. [Thread Communication - `cthreadcomm`](#12-thread-communication--cthreadcomm)
13. [LRU Cache - `clrucache`](#13-lru-cache--clrucache)
14. [Structured Logger - `clogger`](#14-structured-logger--clogger)
15. [JSON Parser / Serializer / DOM - `cjson`](#15-json-parser--serializer--dom--cjson)
16. [YAML Parser / Serializer / DOM - `cyaml`](#16-yaml-parser--serializer--dom--cyaml)
17. [Thread Pool - `cthreadpool`](#17-thread-pool--cthreadpool)
18. [HTTP Client - `chttpclient`](#18-http-client--chttpclient)
19. [HTTP Server - `chttpserver`](#19-http-server--chttpserver)
20. [Thread Safety](#20-thread-safety)
21. [Custom Memory Management](#21-custom-memory-management)
22. [License](#22-license)

---

## 1. Rationale

C gives you direct control over memory, near-zero runtime overhead, and programs that run on everything from microcontrollers to supercomputers. What it does not give you, unfortunately, is a standard library of generic containers.

In C++, `std::vector<int> scores;` gives you a resizable, typed dynamic array. In Java, `new ArrayList<Integer>()` gives you the same thing. In Python, `scores = []` gives you a resizable list that grows on demand. In C, the closest built-in equivalent is a fixed-size array whose size you must know at compile time. Growing it means calling `realloc` yourself. A hash map means implementing one from scratch or tracking down a library. This is a valuable learning exercise, but in a real program you usually want to spend your energy on the problem you are actually solving, not on reimplementing containers you have already studied.

The challenge with generic containers in C is catching type mistakes at compile time. The classic approach passes everything as `void *` (a pointer to untyped memory) which works with any element type but means the compiler cannot warn you about a mismatch. A `double *` silently passed where an `int *` is expected compiles without a warning and produces garbage at runtime. This library uses a C11 feature called `_Generic` that lets a macro inspect the static type of its argument at compile time and dispatch to different code accordingly. The result is that accidental type mismatches (the kind you make by mistake rather than by deliberate cast) are caught at the call site before the program runs.

---

## 2. How Generic Containers Work in C

C does not have built-in support for generic containers. Getting a container to work with any element type requires a trade-off between catching type mistakes at compile time and convenience, and most approaches give up one to gain the other.

The most common approach uses `void *` to erase type information. A container stores a typeless pointer to each element, and the caller casts it back to the right type on retrieval. This compiles for any element type without any changes to the container, but the compiler cannot warn you about type errors. A `double *` silently passed where an `int *` belongs will compile and produce wrong results at runtime.

A second approach uses preprocessor token-pasting to generate a new family of typed functions for each element type. This recovers compile-time type checking, but adding a new type combination requires an explicit instantiation declaration, and the expanded macro code is hard to read when debugging.

A third approach generates C source code from a higher-level description using an external tool. This is clean at the API level but adds a step to the build process and breaks the direct edit-compile-run cycle.

This library uses C11's built-in `_Generic` expression instead. A `_Generic` expression dispatches to different code branches at compile time based on the type of its argument, with no extra tools and no generated files. The macros look like typed containers, behave like typed containers, and produce a compiler error if you use them with the wrong type. The cost is a requirement for a C11-capable compiler with GNU extensions; a reasonable constraint on any modern development machine.

---

## 3. Design Principles

### 3.1 Compile-Time Type Dispatch

C11 introduced a built-in expression called `_Generic` that selects different code branches at compile time based on the static type of a sub-expression. Every container macro in this library uses `_Generic` to inspect the type of its argument at the call site and dispatch to the correct internal path. This catches *accidental* type mismatches (the kind you make without thinking) at compile time, before the program runs.

It is worth being precise about what this provides and what it does not. If you write:

```c
cvec_construct(scores, int);
double d = 3.14;
cvec_push(scores, *(int *)&d);   /* explicit cast; compiles, stores garbage */
```

the cast fools the `_Generic` check and nothing stops you. The library provides compile-time type *dispatch*, not type *safety* in the strict sense: it catches mistakes at the call site as long as you are not actively subverting the type system with a cast. That is enough to eliminate the most common class of bugs while adding zero runtime overhead.

Every container macro inspects its argument with `_Generic` at the call site and records a `ccol_data_type` enum in the container's header struct. This enum drives all subsequent type-dependent decisions at runtime:

- `chashmap` selects open-addressing or separate-chaining based on key and value types.
- `cbstmap` selects signed, unsigned, floating-point, or lexicographic key comparison.
- `csort` selects the default comparator.
- Internal serialisation into `cmap_pair` chooses the correct path.

The key macros are defined in `include/common.h`: `is_integral_type()`, `is_char_ptr()`, `is_char_array()`, and `determine_ccol_data_type()`.

### 3.2 Container Lifecycle Macros

Every use of heap-allocated data in C requires pairing each `malloc` with a corresponding `free`. Forgetting to call `free` causes a memory leak; calling it too early (while the data is still in use) causes a crash. The lifecycle macros provide a consistent discipline across all containers and make it harder to get this wrong.

All containers follow a three-level macro hierarchy that separates declaration, initialisation, and the combination of both:

| Macro | Purpose |
|---|---|
| `*_declare(name, T)` | Declares the container pointer and a hidden companion type variable |
| `*_init(name, ...)` | Allocates and initialises a previously declared container |
| `*_construct(name, T, ...)` | Declares and initialises in a single step |
| `*_construct_scoped(name, T, ...)` | Same as `*_construct`, but registers automatic destruction via `__attribute__((cleanup(...)))` |
| `*_destroy(&name)` | Destroys the container and sets the pointer to `NULL` |

The `*_scoped` variants require no explicit cleanup call. They are particularly valuable in functions with multiple return paths, where manual cleanup becomes error-prone.

```c
void process(void) {
    cvec_construct_scoped(buffer, int);

    /* buffer is populated and used here */

    /* Automatically destroyed when the function returns, regardless of which path is taken */
}
```

### 3.3 Cross-Scope Type Recovery

When you call `cvec_construct(scores, int)`, the macro creates a hidden local variable alongside `scores` that records the element type `int`. This companion variable is what allows `cvec_push` to know how to store a value and `cvec_at` to know how to retrieve one. Because it is a local variable, it only exists in the scope where `*_construct` was called. If you pass `scores` to another function, the companion variable does not travel with it, and the type-dispatching macros will not compile there without it. The `*_redeclare` macro re-creates the companion variable in the new scope, allowing all type-dispatching macros to function correctly there:

```c
void fill(cvec vec) {
    cvec_redeclare(vec, int);   /* Restore type information for this scope */
    cvec_push_rvalue(vec, 1);
    cvec_push_rvalue(vec, 2);
}

int main(void) {
    cvec_construct(numbers, int);
    fill(numbers);
    cvec_destroy(numbers);
    return 0;
}
```

Omitting `*_redeclare` before using a type-dispatching macro in a new scope is the most common source of mistakes when working with this library.

### 3.4 Error Handling

Errors in this library fall into two broad categories. Programming mistakes (passing `NULL` where a valid pointer is required, or requesting an element at an out-of-bounds index) are handled by calling `fatal_err()`, which prints a diagnostic message and terminates the program. This is intentional: a programming mistake should be loud and obvious rather than silently propagated and discovered much later. When you need to handle an expected failure gracefully (for example, a key that might or might not be in a map), use the underlying raw functions, which return a `ccol_retval_t` value you can inspect.

Functions return `ccol_retval_t`, an enum whose value zero indicates success and whose negative values indicate specific failure conditions:

```c
typedef enum {
    ccol_unexpected_failure                = -18,
    ccol_http_connection_failed            = -17,
    ccol_http_host_resolution_failed       = -16,
    ccol_http_tls_handshake_failed         = -15,
    ccol_http_tls_cert_verification_failed = -14,
    ccol_http_tls_cert_load_failed         = -13,
    ccol_http_too_many_redirects           = -12,
    ccol_http_invalid_url                  = -11,
    ccol_http_transfer_aborted             = -10,
    ccol_msg_too_large                     = -9,
    ccol_container_empty                   = -8,
    ccol_container_full                    = -7,
    ccol_timed_out                         = -6,
    ccol_not_permitted                     = -5,
    ccol_invalid_args                      = -4,
    ccol_key_not_found                     = -3,
    ccol_key_already_present               = -2,
    ccol_not_enough_memory                 = -1,
    ccol_success                           =  0
} ccol_retval_t;
```

The convenience macros call `fatal_err()` on hard errors such as programming mistakes and resource exhaustion. When you need to recover from an expected failure condition, call the underlying functions directly and inspect the return value.

### 3.5 Scoped Raw Pointers

The `*_construct_scoped` macros in section 3.2 only cover the library's own container types. `include/common.h` provides an analogous, lighter-weight pair of macros for a plain heap-allocated pointer that is not one of those containers, using the exact same `__attribute__((cleanup(...)))` mechanism:

| Macro | Purpose |
|---|---|
| `ccol_scoped_ptr(name, type)` | Declares `type *name`, initialised to `NULL`, freed via the default allocator at scope exit |
| `ccol_scoped_ptr_mp(name, type, mmgmt_procs)` | Same, but frees `name`'s final value via `mmgmt_procs` (see [Custom Memory Management](#21-custom-memory-management)) |
| `ccol_scoped_ptr_release(name)` | Returns `name`'s current value and sets `name` to `NULL`, cancelling the pending auto-free |

Assign to the declared pointer normally; whatever value it holds when the enclosing scope ends is freed automatically:

```c
void process(void) {
    ccol_scoped_ptr(buf, char);
    buf = malloc(128);
    if (!buf) return;

    /* buf is used here */

    /* Freed automatically when the function returns, regardless of which path is taken */
}
```

Two things this does not try to solve, both consistent with what any other `__attribute__((cleanup(...)))`-based guard in C can offer:

- Reassigning the pointer mid-scope only schedules its *final* value for the automatic free; an earlier value must still be freed manually before it is overwritten.
- To hand ownership of the pointer out of the enclosing scope (for example, returning it from the function that allocated it) instead of having it freed there, call `ccol_scoped_ptr_release`:

```c
char *build(void) {
    ccol_scoped_ptr(buf, char);
    buf = malloc(128);
    if (!buf) return NULL;

    /* ... populate buf ... */

    return ccol_scoped_ptr_release(buf); /* caller now owns it, buf itself is no longer freed */
}
```

`mmgmt_procs` is stored by reference, not copied, so it must remain valid for at least as long as the scoped pointer's own enclosing scope; in practice this holds naturally, since the two are almost always declared in the same scope.

---

## 4. Building and Linking

Clone the repository and run `make` to produce `libccollections.so`:

```bash
make              # Build the shared library
make test         # Build and run all test suites
make memtest      # Run all tests under Valgrind with full leak checking
make clean        # Remove all build artefacts
```

Individual module test suites can be run in isolation:

```bash
cd tests/cvector && make test
cd tests/chashmap && make test
cd tests/cbstmap  && make test
cd tests/csort    && make test
cd tests/cmempool && make test
cd tests/cthreadcomm && make test
cd tests/clogger      && make test
cd tests/cthreadpool  && make test
cd tests/chttpclient  && make test
cd tests/chttpserver  && make test
```

### Fuzz Testing

Three libFuzzer-based fuzzing harnesses ship alongside the test suites, targeting the `cyaml` YAML parser, the `chttp1_parser` HTTP/1.1 parser, and the `cjson` JSON parser directly. None are part of `make test`, `make build`, or `make all`; all are opt-in. A CI job already runs all of them automatically whenever a pull request is merged into the branch `main`, so running them yourself is not part of the regular development loop; this is for readers who want to dig further into parser-level edge cases on their own, not something every contributor needs to touch.

Building them requires Clang (libFuzzer is a Clang/LLVM feature; GCC does not provide it), regardless of which compiler you use for everything else:

```bash
# YAML parser
cd tests/cyaml
make fuzz                                       # builds ./fuzz_cyaml
./fuzz_cyaml fuzz/corpus                        # fuzzes until interrupted (Ctrl-C)
./fuzz_cyaml fuzz/corpus -max_total_time=1800   # or bound the run to N seconds

# HTTP/1.1 parser (request and response modes are separate targets and corpora)
cd tests/chttpclient
make fuzz_request fuzz_response                 # builds ./fuzz_chttp1_request, ./fuzz_chttp1_response
./fuzz_chttp1_request  fuzz/corpus_request  -max_total_time=900
./fuzz_chttp1_response fuzz/corpus_response -max_total_time=900

# JSON parser (the JSON grammar itself, and the separate dot-separated path
# grammar behind cjson_get/cjson_set/cjson_delete, are separate targets and
# corpora)
cd tests/cjson
make fuzz_parse fuzz_path                       # builds ./fuzz_cjson_parse, ./fuzz_cjson_path
./fuzz_cjson_parse fuzz/corpus_parse -max_total_time=900
./fuzz_cjson_path  fuzz/corpus_path  -max_total_time=900
```

Each corpus directory seeds the fuzzer with a small set of curated byte sequences. Drop `-max_total_time` to run indefinitely; doing so mutates and grows the corpus directory in place with newly-discovered inputs, so `git status` will show new files afterward. Any crash, timeout, or out-of-memory finding is written to a `crash-*`/`timeout-*`/`oom-*` file in the same directory; pass that file as the sole argument (e.g. `./fuzz_cyaml crash-<hash>`) to replay it deterministically once you're ready to debug it.

To link an application against the library:

```bash
gcc -o myapp myapp.c -lccollections -lpthread
```

Include only the headers you need:

```c
#include <cvector.h>
#include <chashmap.h>
#include <cbstmap.h>
#include <cstring.h>
#include <csort.h>
#include <cmempool.h>
#include <cthreadcomm.h>
#include <clrucache.h>
#include <clogger.h>
#include <cjson.h>
#include <cyaml.h>
#include <cthreadpool.h>
#include <chttpclient.h>
#include <chttpserver.h>
```

When linking against `chttpclient` or `chttpserver`, add `-lssl -lcrypto -lm` in addition to `-lpthread` (both are backed by TLS via OpenSSL):

```bash
gcc -o myapp myapp.c -lccollections -lpthread -lssl -lcrypto -lm
```

`cvector.h`, `chashmap.h`, and `cbstmap.h` each automatically include `citerators.h`, so the unified iteration API (`ccol_begin`, `ccol_for_each`, `ccol_iter_declare`, and related macros) is available whenever any one of those container headers is included.

### Compile-Time Configuration

`FORK_SAFETY_REQUIRED` (defined to `1` by default in `common.h`) controls whether `cthreadpool`, `cthreadcomm` (`event_loop`, `circular_queue`, `dynamic_queue`, `channel`), `clogger`, and `chttpserver` compile in their `pthread_atfork()`-based protection against a `fork()` call inheriting one of their internal locks already held by a since-vanished thread. That protection costs real work on every single `fork()` call anywhere in the process, by any thread, for any reason: the registered handlers must lock every currently-live handle's own internal lock before `fork()` is allowed to proceed, then unlock them all again. An application that never calls `fork()` at all, or only ever calls it immediately followed by `exec()` (so the child never touches a handle from this library before its own process image is replaced), gets no benefit from this protection and can build the library with `-DFORK_SAFETY_REQUIRED=0` to remove it entirely:

```bash
make EXTRA_CFLAGS="-DFORK_SAFETY_REQUIRED=0"
```

Every other aspect of these modules' thread safety (locking, concurrent create/destroy safety via the generation-tagged handle tables) is unaffected either way; this switch controls fork() protection alone. With it turned off, calling `fork()` while any of these modules' locks might be held by another thread is the caller's own responsibility to avoid.

The supported way to combine `fork(2)` with a `cthreadcomm`, `cthreadpool`, `clogger`, or `chttpserver` handle is one of two patterns: `fork(2)` before creating the handle, so each resulting process builds and owns its own independently (mirroring nginx's and Apache's own prefork worker models), or create the handle and then call `fork(2)` immediately followed by `exec(3)` (mirroring how Go's own `os/exec` always pairs the two), since the exec'd program replaces the process image entirely and never touches anything this library left behind. Continuing to run library or application code in a forked child, without an intervening `exec(3)`, while a handle created before that fork is still live, is not a supported pattern: no single library linked into a process can vouch for every lock some other one (OpenSSL, the C library's own internals, or anything else sharing the process) might be holding at the instant of the fork, so fork-safety at that scope is not a claim this library, or any other, can honestly make. The `pthread_atfork()`-based protection described above exists regardless, as a defense-in-depth measure that keeps this library's own locks specifically from ever being the source of a hang or corruption; each module's own reference page describes what that measure does for it.

### Quick Start

The following example uses the two most commonly needed modules: a dynamic array and a hash map.

```c
#include <cvector.h>
#include <chashmap.h>
#include <stdio.h>

int main(void) {
    /* Dynamic array of exam scores */
    cvec_construct(scores, int);

    cvec_push_rvalue(scores, 91);
    cvec_push_rvalue(scores, 74);
    cvec_push_rvalue(scores, 88);
    cvec_push_rvalue(scores, 63);

    cvec_sort(scores);   /* sort ascending in place */

    printf("Sorted scores:");
    for (size_t i = 0; i < cvec_size(scores); i++)
        printf(" %d", cvec_at(scores, i));
    printf("\n");   /* 63 74 88 91 */

    cvec_destroy(scores);   /* free all memory */

    /* Map from student name to grade */
    chmap_construct(grades, char*, int);

    int g;
    g = 91; chmap_insert(grades, "Alice", g);
    g = 74; chmap_insert(grades, "Bob",   g);
    g = 88; chmap_insert(grades, "Carol", g);

    printf("Alice: %d\n", chmap_get(grades, "Alice"));   /* 91 */

    /* chmap_get_ptr returns NULL when the key is absent */
    if (!chmap_get_ptr(grades, "Dave"))
        printf("Dave has no grade yet.\n");

    chmap_destroy(grades);
    return 0;
}
```

Each module is fully independent: include only the headers your code needs. The remaining sections cover every module in depth, starting with the simpler containers and building toward the more advanced ones.

## 5. Dynamic Array - `cvector`

`cvector` is a resizable array. Unlike a plain C array (`int arr[100]`), a vector grows automatically when you push more elements than it can currently hold; you do not need to know the final size in advance. Indexed access is O(1) (constant time regardless of the array's size). Insertion at the end is amortised O(1): the array occasionally doubles its capacity, but the average cost per insertion, spread over many insertions, stays constant. Sorting is O(n log n) and stable (equal elements keep their original relative order). The internal capacity is always at least four elements, doubles when the array is full, and halves when occupancy drops below one quarter.

**Header:** `#include <cvector.h>`

### Basic Usage

```c
/* Construct a vector of integers */
cvec_construct(scores, int);

/* Push rvalues */
cvec_push_rvalue(scores, 95);
cvec_push_rvalue(scores, 82);
cvec_push_rvalue(scores, 78);
/* Push lvalues */
int val = 87;
cvec_push(scores, val);
val = 98;
cvec_push(scores, val);

/* Index-based access - cvec_at returns a modifiable lvalue */
printf("First score: %d\n", cvec_at(scores, 0));
cvec_at(scores, 0) = 100;  /* Modify in place */

/* Iteration */
for (size_t i = 0; i < cvec_size(scores); i++) {
    printf("%d\n", cvec_at(scores, i));
}

/* Sort ascending (default comparator is selected by element type) */
cvec_sort(scores);

/* Remove and return the last element */
int last = cvec_pop(scores);

/* Destroy */
cvec_destroy(scores);
```

### Pushing Lvalues and Rvalues

Both `cvec_push` and `cvec_push_rvalue` convert their argument to the vector's declared element type the same way a plain C assignment would before storing it, so a value whose type merely happens to be the same size as the vector's element type (an `int` literal pushed into a `long`-typed vector, or a `float` pushed into an `int`-typed vector) is converted correctly rather than having its raw bytes copied verbatim. The two macros exist for a purely syntactic reason: `cvec_push_rvalue` accepts values with no addressable storage of their own (literals and computed expressions); `cvec_push` is the simpler form for a value you already have in a variable. Use `cvec_push` for addressable variables; `cvec_push_rvalue` for literals and expressions:

```c
int x = 42;
cvec_push(scores, x);              /* lvalue */
cvec_push_rvalue(scores, 42);      /* rvalue literal */
cvec_push_rvalue(scores, x * 2);   /* rvalue expression */
```

### Custom Comparison for Sorting

```c
int descending(const void *a, const void *b) {
    return *(const int *)b - *(const int *)a;
}

cvector_sort_with_comparison_proc(scores, descending);
```

### Scoped Variant

```c
void compute(void) {
    cvec_construct_scoped(temp, double);

    cvec_push_rvalue(temp, 1.5);
    cvec_push_rvalue(temp, 2.5);

    /* Destroyed automatically on return */
}
```

### Real-World Use Case: Viewing Exam Scores One Page at a Time

A simple grading program collects every student's score into a vector, sorts the scores from highest to lowest, and prints only one "page" of results at a time; handy when a class roster is too long to fit on one screen. The vector handles growth automatically, and the scoped variant ensures cleanup even on early return:

```c
typedef struct { char name[32]; int score; } Student;

int cmp_score_desc(const void *a, const void *b) {
    const Student *sa = (const Student *)a;
    const Student *sb = (const Student *)b;
    return (sb->score > sa->score) - (sb->score < sa->score);
}

void print_page(Student *all_students, size_t count,
                size_t page, size_t page_size) {
    cvec_construct_scoped(students, Student);

    for (size_t i = 0; i < count; i++)
        cvec_push(students, all_students[i]);

    cvector_sort_with_comparison_proc(students, cmp_score_desc);

    size_t start = page * page_size;
    size_t end   = start + page_size;
    if (end > cvec_size(students)) end = cvec_size(students);

    for (size_t i = start; i < end; i++) {
        Student s = cvec_at(students, i);
        printf("%-12s %d\n", s.name, s.score);
    }

    /* students is destroyed automatically here regardless of which path was taken */
}
```

`cvec_construct_scoped` registers cleanup via `__attribute__((cleanup))`. Any function with multiple return paths benefits from scoped containers: no `goto cleanup` scaffolding, no risk of leaking on an early return.

### Reference: Core Operations

**Lifecycle**

| Macro | Description |
|---|---|
| `cvec_declare(v, T)` | Declare the variable without initialising it |
| `cvec_declare_scoped(v, T)` | Declare with auto-cleanup via `__attribute__((cleanup(...)))`, without initialising |
| `cvec_redeclare(v, T)` | Restore type information in a new scope after passing the vector across a function boundary |
| `cvec_init(v)` | Initialise a previously declared vector using the default allocator; calls `fatal_err()` on failure |
| `cvec_init_mp(v, mprocs)` | Initialise a previously declared vector with a custom allocator; calls `fatal_err()` on failure |
| `cvec_construct(v, T)` | Declare and initialise in one step using the default allocator |
| `cvec_construct_scoped(v, T)` | Declare, initialise, and register auto-cleanup using the default allocator |
| `cvec_construct_mp(v, T, mprocs)` | Declare and initialise with a custom allocator |
| `cvec_construct_mp_scoped(v, T, mprocs)` | Declare, initialise with a custom allocator, and register auto-cleanup |
| `cvec_reset(v)` | Remove all elements and shrink capacity back to the minimum (4 elements) |
| `cvec_destroy(v)` | Destroy and set pointer to `NULL` |

**Element Access and Modification**

| Macro | Description |
|---|---|
| `cvec_push(v, var)` | Append a copy of `var`, converted to `v`'s declared element type the same way a plain C assignment would; `var` may safely alias into `v`'s own backing buffer (for example `cvec_at(v, i)`); calls `fatal_err()` on failure |
| `cvec_push_rvalue(v, expr)` | Append `expr`, converted to `v`'s declared element type the same way a plain C assignment would; accepts rvalues (literals, computed values); calls `fatal_err()` on failure |
| `cvec_pop(v)` | Remove and return the last element as a value; calls `fatal_err()` if the vector is empty |
| `cvec_at(v, i)` | Return a modifiable lvalue reference to the element at index `i`; `i` is evaluated exactly once; calls `fatal_err()` if `i` is out of bounds |
| `cvec_at_ptr(v, i)` | Return a pointer to the element at index `i`, or `NULL` if `i` is out of bounds (does not terminate) |
| `cvec_size(v)` | Return the number of elements currently stored |
| `cvec_reserve(v, n)` | Pre-allocate capacity for at least `n` elements; `n` is evaluated exactly once; calls `fatal_err()` on failure |
| `cvec_data_ptr(v)` | Return a raw pointer to the internal data array; invalidated by any resize |

**Bulk Operations and Sorting**

| Macro | Description |
|---|---|
| `cvec_append_array(v, arr_ptr, count)` | Append `count` elements from a plain C array in a single operation; `count` is evaluated exactly once; `arr_ptr` may safely point into `v`'s own backing buffer; calls `fatal_err()` on failure |
| `cvec_append_cvec(v_dst, v_src)` | Append all elements of `v_src` to `v_dst`; both must have the same element type; calls `fatal_err()` on failure |
| `cvec_sort(v)` | Sort in place using the default comparator for the element type; calls `fatal_err()` on failure |
| `cvector_sort_with_comparison_proc(v, cmp)` | Sort in place using a caller-supplied comparator (`int cmp(const void *, const void *)`); calls `fatal_err()` on failure |

---

## 6. Dynamic String - `cstring`

`cstring` is a heap-allocated string that grows automatically as you append to it. Unlike a fixed `char` array, you do not need to declare a maximum length in advance. Its internal buffer is always null-terminated, so you can pass it directly to any standard library function that expects a `const char *`. Capacity is always a power of two (minimum 16 bytes) and doubles when more space is needed.

**Header:** `#include <cstring.h>`

### Basic Usage

```c
cstr_construct(s, "Hello, world");

/* Query */
size_t len       = cstr_length(s);
const char *raw  = cstr_c_str(s);    /* Read-only pointer to internal buffer */
char ch          = cstr_at(s, 0);
bool is_empty    = cstr_is_empty(s);

/* Modification */
cstr_append(s, "!");
cstr_prepend(s, ">>> ");
cstr_insert(s, 3, "[inserted]");
cstr_set(s, "replacement content");
cstr_to_upper(s);
cstr_to_lower(s);
cstr_trim(s);                         /* Strip leading and trailing whitespace */
cstr_replace(s, "foo", "bar");        /* Replace all non-overlapping occurrences */

/* Search and comparison */
bool eq        = cstr_equals(s, "hello");
bool starts    = cstr_starts_with(s, "he");
bool ends      = cstr_ends_with(s, "lo");
size_t pos     = cstr_find(s, "ll");  /* Returns ccol_invalid_size if not found */
size_t rpos    = cstr_rfind(s, "l");

/* Derived strings - caller is responsible for destroying these */
cstr sub  = cstr_substring(s, 1, 4);
cstr copy = cstr_copy(s, NULL);
cstr_destroy(sub);
cstr_destroy(copy);

cstr_destroy(s);
```

> **Pointer stability:** `cstr_c_str()` returns a pointer into the string's internal buffer. Any mutating operation (`cstr_append`, `cstr_insert`, `cstr_replace`, and others) may reallocate the buffer, invalidating all previously obtained raw pointers.

### Splitting a Delimited String

`cstr_split` tokenises a string and returns a `cvec` of `cstr` values. Each token is an independently allocated string that must be destroyed by the caller before the vector is destroyed.

```c
cstr_construct(line, "alice,30,engineer");

cvec parts = cstr_split(line, ",", NULL);
cvec_redeclare(parts, cstr);

for (size_t i = 0; i < cvec_size(parts); i++) {
    printf("[%zu] %s\n", i, cstr_c_str(cvec_at(parts, i)));
    cstr_destroy(cvec_at(parts, i));
}
cvec_destroy(parts);
cstr_destroy(line);
```

Output:
```
[0] alice
[1] 30
[2] engineer
```

### Using `cstring` with Maps

`cstr` is not a recognised key or value type in `chashmap` or `cbstmap`. The maps understand `char *` as a distinct type with content-based hashing and comparison. The correct pattern is to extract the internal pointer with `cstr_c_str()` and use that as the key:

```c
chmap_construct(map, char*, int);

cstr_construct(key, "hello");
char *k = (char *)cstr_c_str(key);
int count = 1;
chmap_insert(map, k, count);

/* The map copies the string content into its own storage (via SSO or heap allocation).
   The cstr can be destroyed independently without affecting the map entry. */
cstr_destroy(key);
chmap_destroy(map);
```

### Real-World Use Case: Reading a Simple Settings File

Many small programs (games included) keep their settings in a plain text file with one `key = value` pair per line, plus the occasional `#` comment. Reading a file like this is a good illustration of the string manipulation API: trimming handles inconsistent whitespace, splitting tokenises the line by delimiter, and `cstr_starts_with` skips comment lines cheaply:

```c
void load_config(const char *path, chmap config) {
    chmap_redeclare(config, char*, char*);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        cstr_construct_scoped(line, buf);
        cstr_trim(line);

        if (cstr_is_empty(line) || cstr_starts_with(line, "#"))
            continue;

        cvec parts = cstr_split(line, "=", NULL);
        cvec_redeclare(parts, cstr);
        if (cvec_size(parts) < 2) { cvec_destroy(parts); continue; }

        cstr key = cvec_at(parts, 0);
        cstr val = cvec_at(parts, 1);
        cstr_trim(key);
        cstr_trim(val);

        /* The map copies both strings internally */
        char *k = (char *)cstr_c_str(key);
        char *v = (char *)cstr_c_str(val);
        chmap_insert(config, k, v);

        for (size_t i = 0; i < cvec_size(parts); i++)
            cstr_destroy(cvec_at(parts, i));
        cvec_destroy(parts);
    }
    fclose(f);
}
```

`cstr_construct_scoped` frees the temporary line buffer automatically on each loop iteration. `cstr_split` returns a `cvec` of independently owned `cstr` values; each token must be destroyed before the vector is destroyed.

### Reference: Core Operations

**Lifecycle**

| Macro / Function | Description |
|---|---|
| `cstr_declare(s)` | Declare the variable without initialising it |
| `cstr_declare_scoped(s)` | Declare with auto-cleanup via `__attribute__((cleanup(...)))`, without initialising |
| `cstr_init(s, initial)` | Initialise a previously declared string; `initial` may be a C string or `NULL` for empty; calls `fatal_err()` on failure |
| `cstr_init_mp(s, initial, mprocs)` | Initialise a previously declared string with a custom allocator; calls `fatal_err()` on failure |
| `cstr_construct(s, initial)` | Declare and initialise in one step using the default allocator |
| `cstr_construct_scoped(s, initial)` | Declare, initialise, and register auto-cleanup |
| `cstr_construct_mp(s, initial, mprocs)` | Declare and initialise with a custom allocator |
| `cstr_construct_mp_scoped(s, initial, mprocs)` | Declare, initialise with a custom allocator, and register auto-cleanup |
| `cstring_new(initial)` | Allocate and return a new `cstr` using default allocators; returns `NULL` on failure with no error detail (equivalent to `cstring_create_full(initial, NULL, NULL)`) |
| `cstr_reserve(s, cap)` | Pre-allocate at least `cap` bytes (rounded up to next power of two, minimum 16); calls `fatal_err()` on failure |
| `cstr_reset(s)` | Clear all characters and shrink capacity back to the minimum |
| `cstr_destroy(s)` | Destroy and set pointer to `NULL` |

**Query**

| Macro / Function | Description |
|---|---|
| `cstr_length(s)` | Return the number of characters, excluding the null terminator |
| `cstr_c_str(s)` | Return a read-only pointer to the null-terminated internal buffer; invalidated by any mutating operation |
| `cstr_at(s, idx)` | Return the character at zero-based index `idx`; returns `'\0'` if out of bounds |
| `cstr_is_empty(s)` | Return `true` if the string contains no characters |

**Modification**

| Macro / Function | Description |
|---|---|
| `cstr_append(s, str)` | Append C string `str` to the end; calls `fatal_err()` on failure |
| `cstr_prepend(s, str)` | Prepend C string `str` to the beginning; calls `fatal_err()` on failure |
| `cstr_insert(s, pos, str)` | Insert C string `str` at zero-based position `pos`; calls `fatal_err()` on failure |
| `cstr_set(s, str)` | Replace the entire content with C string `str`; calls `fatal_err()` on failure |
| `cstr_to_upper(s)` | Convert all characters to uppercase in place |
| `cstr_to_lower(s)` | Convert all characters to lowercase in place |
| `cstr_trim(s)` | Strip leading and trailing whitespace in place (classified by `isspace()`) |
| `cstr_replace(s, needle, replacement)` | Replace every non-overlapping occurrence of `needle` with `replacement`; calls `fatal_err()` on failure |

**Search and Comparison**

| Macro / Function | Description |
|---|---|
| `cstr_compare(s, str)` | Lexicographic comparison against C string `str`; semantics identical to `strcmp()` |
| `cstr_equals(s, str)` | Return `true` if the content equals C string `str` |
| `cstr_starts_with(s, prefix)` | Return `true` if the string begins with `prefix`; empty prefix always matches |
| `cstr_ends_with(s, suffix)` | Return `true` if the string ends with `suffix`; empty suffix always matches |
| `cstr_find(s, needle)` | Return the zero-based index of the first occurrence of `needle`, or `ccol_invalid_size` if not found |
| `cstr_rfind(s, needle)` | Return the zero-based index of the last occurrence of `needle`, or `ccol_invalid_size` if not found |

**Derived Strings**

| Macro / Function | Description |
|---|---|
| `cstr_substring(s, start, len)` | Create and return a new `cstr` containing `len` characters starting at `start`; range is clamped to string bounds; caller must destroy the result; calls `fatal_err()` on failure |
| `cstr_copy(s, err)` | Create and return an independent copy of the string; caller must destroy the result |
| `cstr_split(s, delim, err)` | Tokenise the string by `delim` and return a `cvec` of `cstr` values; caller must destroy each token and then the vector |

---

## 7. Hash Map - `chashmap`

A hash map stores key-value pairs and answers "what value is associated with this key?" in constant time on average (O(1)), regardless of how many pairs are stored. If you have used C++'s `std::unordered_map`, Java's `HashMap`, or Python's `dict`, this is the same concept.

`chashmap` selects one of two internal strategies at construction time based on the key and value types, but the macro API is identical for both.

**Header:** `#include <chashmap.h>`

### Implementation Selection

**Open-addressing** is selected when both the key and the value are integral types no wider than eight bytes. It uses compact 24-byte slots (8-byte key, 8-byte value, 1-byte metadata, padded to a multiple of 8 so key/value storage stays naturally aligned for direct in-place access), Fibonacci hashing for integers, and linear probing. Load factor thresholds are 0.70 (grow) and 0.25 (shrink), with a 2x scale factor. There are zero per-entry heap allocations, and cache locality is quite good.

**Separate chaining** is selected for all other type combinations. It uses a linked-list per bucket, Small String Optimisation (23-byte inline buffer for short strings), and a doubly-linked list that preserves reverse insertion order. The minimum bucket count is 16 (always a power of two), and the scale factor is 4x.

The default hash function is selected by key type, not by which backend ends up chosen: Fibonacci hashing for integral/float/double/pointer keys, XXHash64 for string and other buffer-like keys. A separate-chaining map with an integral key type (for example a `double` key paired with a non-integral value) still gets Fibonacci hashing for that key.

The selection happens transparently; the same macro interface is used in both cases. `chmap_get_ptr`/`chmap_get_elem_ref` always return a pointer that is correctly aligned for the value's type, regardless of which backend is in use. A reference returned by `chmap_get_elem_ref` stays valid until the map is actually modified (insert/delete/resize); looking up a different key never invalidates a reference already held for another key, so multiple references may be kept concurrently. A `key_pair` whose size does not match the byte size of the key type is rejected with `ccol_invalid_args` for any fixed-width key type (every integral type, `float`, `double`, and `long double`), regardless of which backend the map uses; a `val_pair` whose size does not match the byte size of the value type is rejected the same way, but only on a map using the open-addressing backend, since separate chaining accepts values of varying size.

### Basic Usage

```c
/* Both key and value are integral - open-addressing is selected */
chmap_construct(counters, int, long);

/* String key - separate chaining is selected */
chmap_construct(index, char*, int);

/* Insertion: keys and values must be lvalues (see note below) */
int id = 42;
long count = 1000;
chmap_insert(counters, id, count);

int freq = 5;
chmap_insert(index, "hello", freq);

/* Retrieval */
int lookup = 42;
long val = chmap_get(counters, lookup);          /* Fatal error if key absent */
long *ptr = chmap_get_ptr(counters, lookup);     /* Returns NULL if key absent */

/* Removal */
int remove_key = 42;
ccol_retval_t rc = chmap_remove(counters, remove_key);

/* Destroy */
chmap_destroy(counters);
chmap_destroy(index);
```

> **Lvalue requirement:** The insertion macros take the address of both the key and the value with the `&` operator. Integer and floating-point literals, as well as computed expressions, have no addressable storage and will not compile as map arguments. Assign them to variables first. String literals are the sole exception, as they are statically addressable.

### Iteration

The `ccol_for_each` macro is the recommended iteration pattern. It declares the iterator variable in its own scope, avoiding name collisions:

```c
ccol_for_each(index, it, {
    printf("%s: %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});
```

When direct control over iteration is required, the iterator can be managed manually. Note that the iterator variable must be declared outside the loop:

```c
ccol_iter_declare(index, it);
for (it = ccol_begin(index); it != NULL; it = ccol_iter_next(it)) {
    printf("%s -> %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
}
```

The unified iteration macros are provided by `citerators.h`, which is automatically included when you include `chashmap.h`. See [Section 9](#9-unified-iteration--citerators) for the full API reference.

Iteration order differs by implementation: separate chaining iterates in reverse insertion order via its internal doubly-linked list; open-addressing iterates in slot order, which can be considered random.

### Example: Word Frequency Count

```c
chmap_construct(freq, char*, int);

const char *words[] = {"the", "cat", "sat", "on", "the", "mat", "the"};
size_t n = sizeof(words) / sizeof(words[0]);

for (size_t i = 0; i < n; i++) {
    char *w = (char *)words[i];
    int *p  = chmap_get_ptr(freq, w);
    if (p) {
        (*p)++;
    } else {
        int one = 1;
        chmap_insert(freq, w, one);
    }
}

ccol_for_each(freq, it, {
    printf("%-8s %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});

chmap_destroy(freq);
```

### Memory Ownership for String Keys and Values

When the key or value type is `char *`, the map copies the string content into its own storage (inline if it fits in 23 bytes, heap-allocated otherwise). The original pointer may be freed immediately after insertion without affecting the map. Retrieved string pointers point into the map's internal storage and must not be freed by the caller.

For non-string pointer values (such as `Point *`) the map stores the pointer itself, not a copy of the pointed-to object. Lifetime management of the pointed-to data is the caller's responsibility:

```c
typedef struct { int x, y; } Point;
chmap_construct(coords, int, Point*);

Point *p = malloc(sizeof(Point));
p->x = 10; p->y = 20;
int key = 1;
chmap_insert(coords, key, p);

/* Before destroying the map, free all pointed-to objects */
ccol_for_each(coords, it, {
    free(*ccol_iter_val_ptr(it));
});
chmap_destroy(coords);
```

### Custom Hashing Functions

`chmap_create_ch`/`chmap_construct_ch` (and the `_full` variants) accept a custom hash function of type `ccol_hashing_proc_t`, which receives both the key's address and its size in bytes, so it can hash a variable-length binary key (a string, or any other buffer-like key) without relying on a NUL terminator or other external convention to find its own extent:

```c
unsigned long fnv1a_hash(const void *ptr, size_t size) {
    const unsigned char *b = (const unsigned char *)ptr;
    unsigned long h = 2166136261UL;
    for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 16777619UL;
    return h;
}

chmap_construct_ch(m, char*, int, fnv1a_hash);
```

For a fixed-size key type (an integer, `float`, `double`, a pointer), `size` is always that type's own `sizeof`.

### Key Equality for `float` and `double` Keys

Key equality for a `float` or `double` key is bitwise equality, not IEEE 754 `==` equality, with one exception: `-0.0` and `0.0` are canonicalized to the same key, matching `-0.0 == 0.0` in C. Every other bit pattern, including NaN, follows bitwise equality instead of `==` semantics: a NaN key is always found again by the exact bit pattern it was inserted with (unlike `NaN == NaN`, which is always false in C), and two different NaN payloads are two different keys.

This canonicalization happens before the key is hashed, stored, or compared, and applies unconditionally, including with a custom hashing function passed via `chmap_create_ch`/`chmap_construct_ch`: the custom function is always given the already-canonicalized bit pattern, and a key inserted as `-0.0` is stored (and later observed via iteration) as `0.0`, not its original bit pattern.

```c
chmap_construct(m, double, int);

double neg_zero = -0.0, pos_zero = 0.0;
int v = 1;
chmap_insert(m, neg_zero, v);
chmap_get(m, pos_zero);   /* 1 - -0.0 and 0.0 are the same key */
```

### Key Equality for `long double` Keys

A `long double` key (always separate chaining; see Implementation Selection above) is compared by numeric value (native `==`), not bitwise equality: `-0.0L` and `0.0L` are the same key, matching `-0.0L == 0.0L` in C, and two keys holding the identical value remain the same key even if their in-memory representations differ in ways that do not affect the value itself. This differs from `float`/`double`, whose equality is bitwise with only `-0.0`/`0.0` unified; `long double` needs the value-based approach because its representation is not fully significant on most platforms (extra bytes beyond the actual precision have no defined meaning), so two variables holding the same number are not guaranteed to be byte-identical the way a `float`/`double` always is.

Every NaN `long double` collapses into a single key, regardless of its payload; this is a deliberate difference from `float`/`double`'s own per-payload NaN identity, made necessary by the same representation gap noted above.

This value-based handling is specific to the map's default hashing; a custom hashing function passed via `chmap_create_ch`/`chmap_construct_ch` receives the key's raw bytes exactly like it would for any other type, so a custom hash function for a `long double` key type must itself be value-based (for example, by hashing the result of `frexpl()`) to stay consistent with the map's own key equality.

```c
chmap_construct(m, long double, int);

long double a = 3.0L;
long double b = 1.0L + 2.0L;   /* same value, computed differently */
int v = 1;
chmap_insert(m, a, v);
chmap_get(m, b);   /* 1 - a and b are the same key */
```

### Real-World Use Case: Command Dispatcher for a Text Adventure Game

A small text adventure game reads a word typed by the player ("look", "inventory", "quit") and needs to run the matching function. Storing each command as a string key mapped to a function pointer turns this into a single O(1) map lookup instead of a long chain of `if (strcmp(...))` comparisons. `chmap_get_ptr` returns `NULL` for a command the game does not recognise, without triggering a fatal error:

```c
typedef void (*command_fn)(void);

void cmd_look(void)      { printf("You see a dusty room and a locked door.\n"); }
void cmd_inventory(void) { printf("You are carrying: a torch, a rusty key.\n"); }
void cmd_quit(void)      { printf("Goodbye!\n"); }

void commands_init(chmap commands) {
    chmap_redeclare(commands, char*, command_fn);

    command_fn f;
    f = cmd_look;      chmap_insert(commands, "look",      f);
    f = cmd_inventory; chmap_insert(commands, "inventory", f);
    f = cmd_quit;      chmap_insert(commands, "quit",      f);
}

void run_command(chmap commands, const char *typed_word) {
    chmap_redeclare(commands, char*, command_fn);

    char key[64];
    snprintf(key, sizeof(key), "%s", typed_word);

    command_fn *fn = chmap_get_ptr(commands, key);
    if (!fn) {
        printf("I don't understand \"%s\".\n", typed_word);
        return;
    }
    (*fn)();
}
```

The map copies each key string into its own storage (inline for strings up to 23 bytes, heap-allocated otherwise), so the temporary `key` buffer on the stack is safe after insertion.

### Reference: Core Operations

**Lifecycle**

| Macro | Description |
|---|---|
| `chmap_declare(m, K, V)` | Declare the variable without initialising it |
| `chmap_declare_scoped(m, K, V)` | Declare with auto-cleanup via `__attribute__((cleanup(...)))`, without initialising |
| `chmap_redeclare(m, K, V)` | Restore type information in a new scope after passing the map across a function boundary |
| `chmap_init(m)` | Initialise a previously declared map using the default allocator and default hash; calls `fatal_err()` on failure |
| `chmap_init_mp(m, mprocs)` | Initialise with a custom allocator and default hash; calls `fatal_err()` on failure |
| `chmap_init_ch(m, hash_fn)` | Initialise with the default allocator and a custom hash function; calls `fatal_err()` on failure |
| `chmap_init_full(m, mprocs, hash_fn)` | Initialise with a custom allocator and a custom hash function; calls `fatal_err()` on failure |
| `chmap_construct(m, K, V)` | Declare and initialise in one step using the default allocator and default hash |
| `chmap_construct_scoped(m, K, V)` | Declare, initialise, and register auto-cleanup |
| `chmap_construct_mp(m, K, V, mprocs)` | Declare and initialise with a custom allocator |
| `chmap_construct_mp_scoped(m, K, V, mprocs)` | Declare, initialise with a custom allocator, and register auto-cleanup |
| `chmap_construct_ch(m, K, V, hash_fn)` | Declare and initialise with a custom hash function |
| `chmap_construct_ch_scoped(m, K, V, hash_fn)` | Declare, initialise with a custom hash function, and register auto-cleanup |
| `chmap_construct_full(m, K, V, mprocs, hash_fn)` | Declare and initialise with a custom allocator and a custom hash function |
| `chmap_construct_full_scoped(m, K, V, mprocs, hash_fn)` | Declare, initialise with a custom allocator and a custom hash function, and register auto-cleanup |
| `chmap_reset(m, new_size)` | Remove all entries and resize the bucket array to `new_size`; pass `0` to keep the current bucket count |
| `chmap_destroy(m)` | Destroy and set pointer to `NULL` |

**Element Operations**

| Macro / Function | Description |
|---|---|
| `chmap_insert(m, key, val)` | Insert or update (upsert); keys and values must be lvalues; calls `fatal_err()` on non-key-collision failure |
| `chmap_get(m, key)` | Return the value associated with `key`; calls `fatal_err()` if the key is absent |
| `chmap_get_ptr(m, key)` | Return a pointer to the value, or `NULL` if the key is absent; pointer is invalidated by any subsequent insert, remove, or resize |
| `chmap_remove(m, key)` | Remove the entry for `key`; returns `ccol_success` or `ccol_key_not_found` |
| `chmap_elem_count(m)` | Return the number of entries currently stored |

**Iteration**

These macros come from `citerators.h`, which `chashmap.h` includes automatically. See [Section 9](#9-unified-iteration--citerators) for the full reference.

| Macro | Description |
|---|---|
| `ccol_for_each(m, it, { })` | Recommended iteration pattern; declares the iterator in its own scope and traverses all entries |
| `ccol_iter_declare(m, it)` | Declare a manual iterator variable with RAII cleanup; required before using `ccol_begin` in a `for` loop |
| `ccol_begin(m)` | Return an iterator positioned at the first entry, or `NULL` if the map is empty; calls `fatal_err()` on allocation failure |
| `ccol_iter_next(it)` | Advance to the next entry; returns `NULL` at the end and automatically destroys the iterator |
| `ccol_iter_key_ptr(it)` | Return a typed const pointer to the current entry's key |
| `ccol_iter_val_ptr(it)` | Return a typed pointer to the current entry's value; the value may be modified in place |
| `ccol_iter_destroy(it)` | Destroy a manual iterator before it reaches the end; sets pointer to `NULL` |

---

## 8. Ordered Map - `cbstmap`

An ordered map works like a hash map (you look up values by key) but it always keeps its keys in sorted order. Iterating over it visits entries from smallest key to largest. This makes it the right choice when you need both fast lookup and ordered traversal.

`cbstmap` is implemented as a self-balancing AVL tree, which guarantees O(log n) worst-case performance for insertion, deletion, and lookup regardless of the order in which keys are inserted. (A naive binary search tree degrades to O(n) on sorted input; the AVL rebalancing prevents that.)

**Header:** `#include <cbstmap.h>`

Automatic key comparison is provided for `char` keys (compared using this platform's own native `char` semantics, signed or unsigned), signed integer keys (`signed char`/`int8_t`, `short`, `int`, `long`, `long long`, always compared as genuinely signed regardless of this platform's own `char` signedness), unsigned integer keys, floating-point keys (`float`, `double`, `long double`, compared by numeric value), and `char *` keys (using `strcmp`). For other key types (structs, enums, and the like), a custom comparison function must be supplied, or the default falls back to a raw `memcmp` of the key's representation (see the callout below). A `NaN` floating-point key sorts as greater than every non-NaN key and equal to every other NaN key, so a `NaN` key can be inserted, looked up, and deleted like any other key.

### Basic Usage

```c
/* Signed integer key: signed comparison path is selected automatically */
cbmap_construct(registry, int, char*);

/* Insertion */
int id = 100;
cbmap_insert(registry, id, "Alice");

id = 50;
cbmap_insert(registry, id, "Bob");

id = 200;
cbmap_insert(registry, id, "Charlie");

/* Retrieval */
int lookup = 100;
char *name  = cbmap_get(registry, lookup);          /* Fatal error if absent */
char **ptr  = cbmap_get_ptr(registry, lookup);      /* NULL if absent */

/* Removal */
int remove_key = 50;
ccol_retval_t rc = cbmap_remove(registry, remove_key);

/* Destroy */
cbmap_destroy(registry);
```

### Iteration in Sorted Order

```c
cbmap_construct(scores, int, char*);

int s = 78; cbmap_insert(scores, s, "Charlie");
s = 91;     cbmap_insert(scores, s, "Diana");
s = 82;     cbmap_insert(scores, s, "Bob");
s = 95;     cbmap_insert(scores, s, "Alice");

/* In-order traversal visits entries from score 78 to 95 */
ccol_for_each(scores, it, {
    printf("%3d  %s\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});

cbmap_destroy(scores);
```

Output:
```
 78  Charlie
 82  Bob
 91  Diana
 95  Alice
```

### Custom Key Comparison

```c
typedef struct { uint32_t major; uint32_t minor; } Version;

int compare_version(const void *a, const void *b) {
    const Version *va = (const Version *)a;
    const Version *vb = (const Version *)b;
    if (va->major != vb->major)
        return (va->major > vb->major) - (va->major < vb->major);
    return (va->minor > vb->minor) - (va->minor < vb->minor);
}

cbmap_construct_cc(changelog, Version, char*, compare_version);
```

> **Struct keys without a custom comparator:** `cbmap_construct` (no `_cc`/`_ch` suffix) falls back to a raw byte-for-byte `memcmp` of the key's representation when the key type is neither an integer nor `char *`. For a struct, this compares padding bytes (which are indeterminate for a stack-allocated or partially-initialized struct) and any pointer members by their raw address rather than by what they point to, so the resulting order rarely matches a struct's intended field-by-field ordering and is not guaranteed to be consistent across two structs that an application would otherwise consider equal. Provide a comparator (`cbmap_construct_cc`, shown above) for any struct key type.

### String Keys

`char *` keys are natively supported with `strcmp`-based comparison. No custom comparator is needed:

```c
cbmap_construct(env, char*, char*);

char *k = "HOME";
cbmap_insert(env, k, "/home/user");

k = "PATH";
cbmap_insert(env, k, "/usr/local/bin:/usr/bin");

cbmap_destroy(env);
```

### Real-World Use Case: Live Leaderboard

A simple game keeps a leaderboard where players are ranked by score. Storing negated scores as keys causes the AVL tree's ascending in-order traversal to visit entries from highest to lowest score. Insertion and lookup are both O(log n); the tree rebalances automatically:

```c
void leaderboard_upsert(cbmap board, const char *player, int score) {
    cbmap_redeclare(board, int, char*);
    int neg_score = -score;
    cbmap_insert(board, neg_score, player);
}

void leaderboard_print_top(cbmap board, size_t n) {
    cbmap_redeclare(board, int, char*);
    size_t rank = 1;
    ccol_for_each(board, it, {
        if (rank > n) { ccol_iter_destroy(it); break; }
        printf("#%zu  %-20s  %d pts\n",
               rank,
               *ccol_iter_val_ptr(it),
               -(*ccol_iter_key_ptr(it)));   /* un-negate for display */
        rank++;
    });
}
```

The AVL self-balancing property keeps the tree height bounded at O(log n) even under adversarial insertion patterns such as scores arriving in strictly ascending order, where a naive BST would degrade to a linked list.

### Reference: Core Operations

**Lifecycle**

| Macro | Description |
|---|---|
| `cbmap_declare(m, K, V)` | Declare the variable without initialising it |
| `cbmap_declare_scoped(m, K, V)` | Declare with auto-cleanup via `__attribute__((cleanup(...)))`, without initialising |
| `cbmap_redeclare(m, K, V)` | Restore type information in a new scope after passing the map across a function boundary |
| `cbmap_init(m)` | Initialise a previously declared map using the default allocator and automatic key comparison; calls `fatal_err()` on failure |
| `cbmap_init_mp(m, mprocs)` | Initialise with a custom allocator and automatic key comparison; calls `fatal_err()` on failure |
| `cbmap_init_cc(m, cmp_fn)` | Initialise with the default allocator and a custom comparison function; calls `fatal_err()` on failure |
| `cbmap_init_full(m, mprocs, cmp_fn)` | Initialise with a custom allocator and a custom comparison function; calls `fatal_err()` on failure |
| `cbmap_construct(m, K, V)` | Declare and initialise in one step; key comparison is selected automatically from the key type |
| `cbmap_construct_scoped(m, K, V)` | Declare, initialise, and register auto-cleanup |
| `cbmap_construct_mp(m, K, V, mprocs)` | Declare and initialise with a custom allocator |
| `cbmap_construct_mp_scoped(m, K, V, mprocs)` | Declare, initialise with a custom allocator, and register auto-cleanup |
| `cbmap_construct_cc(m, K, V, cmp_fn)` | Declare and initialise with a custom comparison function; required for key types that are not natively supported |
| `cbmap_construct_cc_scoped(m, K, V, cmp_fn)` | Declare, initialise with a custom comparison function, and register auto-cleanup |
| `cbmap_construct_full(m, K, V, mprocs, cmp_fn)` | Declare and initialise with a custom allocator and a custom comparison function |
| `cbmap_construct_full_scoped(m, K, V, mprocs, cmp_fn)` | Declare, initialise with a custom allocator and a custom comparison function, and register auto-cleanup |
| `cbmap_reset(m)` | Remove all entries; the tree structure remains valid for reuse |
| `cbmap_destroy(m)` | Destroy and set pointer to `NULL` |

**Element Operations**

| Macro / Function | Description |
|---|---|
| `cbmap_insert(m, key, val)` | Insert or update (upsert); keys and values must be lvalues; tree is rebalanced automatically; calls `fatal_err()` on non-key-collision failure |
| `cbmap_get(m, key)` | Return the value associated with `key`; calls `fatal_err()` if the key is absent; O(log n) |
| `cbmap_get_ptr(m, key)` | Return a pointer to the value, or `NULL` if the key is absent; pointer is invalidated by any subsequent insert or remove; O(log n) |
| `cbmap_remove(m, key)` | Remove the entry for `key`; tree is rebalanced automatically; returns `ccol_success` or `ccol_key_not_found`; O(log n) |
| `cbmap_elem_count(m)` | Return the number of entries currently stored |

**Iteration**

These macros come from `citerators.h`, which `cbstmap.h` includes automatically. See [Section 9](#9-unified-iteration--citerators) for the full reference.

| Macro | Description |
|---|---|
| `ccol_for_each(m, it, { })` | Recommended iteration pattern; declares the iterator in its own scope and traverses all entries in ascending key order |
| `ccol_iter_declare(m, it)` | Declare a manual iterator variable with RAII cleanup; required before using `ccol_begin` in a `for` loop |
| `ccol_begin(m)` | Return an iterator positioned at the entry with the smallest key, or `NULL` if the map is empty |
| `ccol_iter_next(it)` | Advance to the next entry in sorted key order; returns `NULL` at the end and automatically destroys the iterator |
| `ccol_iter_key_ptr(it)` | Return a typed const pointer to the current entry's key |
| `ccol_iter_val_ptr(it)` | Return a typed pointer to the current entry's value; the value may be modified in place |
| `ccol_iter_destroy(it)` | Destroy a manual iterator before it reaches the end; sets pointer to `NULL` |

---

## 9. Unified Iteration - `citerators`

`citerators.h` provides a single iteration API that works identically across `cvector`, `chashmap`, and `cbstmap`. You do not need to learn a different loop pattern for each container type. There is no need to include this header explicitly: each of the three container headers pulls it in automatically.

**Header:** included automatically by `cvector.h`, `chashmap.h`, and `cbstmap.h`.

### `ccol_for_each` - The Recommended Pattern

```c
chmap_construct(word_count, char*, int);
/* ... populate ... */

ccol_for_each(word_count, it, {
    printf("%-12s %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});

chmap_destroy(word_count);
```

The macro declares the iterator variable `it` in its own block scope. The iterator is destroyed automatically when the loop ends (whether by reaching the end or by `break`), so no explicit cleanup is needed.

### Manual Iterator Management

When you need early termination, multiple passes, or conditional logic that does not fit cleanly into a body block, manage the iterator by hand:

```c
cbmap_construct(registry, int, char*);
/* ... populate ... */

ccol_iter_declare(registry, it);
for (it = ccol_begin(registry); it != NULL; it = ccol_iter_next(it)) {
    if (strcmp(*ccol_iter_val_ptr(it), "target") == 0) {
        ccol_iter_destroy(it);
        break;
    }
    printf("%d\n", *ccol_iter_key_ptr(it));
}

cbmap_destroy(registry);
```

`ccol_iter_declare` attaches `__attribute__((cleanup))` to the iterator variable so it is destroyed automatically on scope exit even if `ccol_iter_destroy` is not called explicitly.

### Iterating a Vector

For `cvector`, the iterator key is the element index (`size_t`) and the iterator value is the element itself:

```c
cvec_construct(scores, int);
cvec_push_rvalue(scores, 95);
cvec_push_rvalue(scores, 82);
cvec_push_rvalue(scores, 78);

ccol_for_each(scores, it, {
    printf("[%zu] = %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});

cvec_destroy(scores);
```

Output:
```
[0] = 95
[1] = 82
[2] = 78
```

### Real-World Use Case: Printing Any Product's Details

A small shop-inventory program wants one function that can print the details of any product, whether it is a shirt with a color and a size or a mug with a color and a material. Since every product's fields are stored in a `chmap`, the same printing function works for all of them without knowing the field names in advance. `chmap_redeclare` restores the companion type variables in the new scope, and `ccol_for_each` handles the rest:

```c
void print_product(const char *name, chmap fields) {
    chmap_redeclare(fields, char*, char*);

    cstr_construct_scoped(line, name);
    cstr_append(line, ": ");

    ccol_for_each(fields, it, {
        cstr_append(line, *ccol_iter_key_ptr(it));
        cstr_append(line, "=");
        cstr_append(line, *ccol_iter_val_ptr(it));
        cstr_append(line, " ");
    });

    printf("%s\n", cstr_c_str(line));
}

int main(void) {
    chmap_construct(shirt, char*, char*);
    chmap_insert(shirt, "color", "blue");
    chmap_insert(shirt, "size",  "L");

    chmap_construct(mug, char*, char*);
    chmap_insert(mug, "color",    "white");
    chmap_insert(mug, "material", "ceramic");

    print_product("shirt", shirt);   /* shirt: color=blue size=L */
    print_product("mug",   mug);     /* mug: color=white material=ceramic */

    chmap_destroy(shirt);
    chmap_destroy(mug);
    return 0;
}
```

The same `ccol_for_each` / `ccol_iter_next` / `ccol_iter_key_ptr` / `ccol_iter_val_ptr` surface works identically on `cvector`, `chmap`, and `cbmap`. The dispatch to the correct begin/next functions is resolved at compile time by `ccol_begin` via `_Generic`, with zero runtime overhead.

### Reference: Core Operations

| Macro | Header | Description |
|---|---|---|
| `ccol_begin(container)` | `citerators.h` | Return an iterator at the first element, or `NULL` if empty; calls `fatal_err()` on allocation failure |
| `ccol_end` | `citerators.h` | The end sentinel: `NULL` |
| `ccol_for_each(container, it, { })` | `citerators.h` | Declare `it`, loop from `ccol_begin` to `ccol_end`, destroy on exit |
| `ccol_iter_declare(container, it)` | `citerators.h` | Declare a typed iterator variable with RAII cleanup; required before using `ccol_begin` in a `for` loop |
| `ccol_iter_next(it)` | `citerators.h` | Advance to the next element; returns `NULL` at the end and destroys the iterator |
| `ccol_iter_key_ptr(it)` | `citerators.h` | Return a typed `const KeyT *` to the current key (index for vectors, map key for maps) |
| `ccol_iter_val_ptr(it)` | `citerators.h` | Return a typed `ValT *` to the current value; mutations are reflected in the container |
| `ccol_iter_destroy(it)` | `citerators.h` | Destroy the iterator before reaching the end; sets it to `NULL` |

**Container-to-iterator key type mapping:**

| Container | Key type returned by `ccol_iter_key_ptr` | Value type returned by `ccol_iter_val_ptr` |
|---|---|---|
| `cvec` | `const size_t *` (element index) | `const ElemT *` |
| `chmap` | `const KeyT *` | `ValT *` |
| `cbmap` | `const KeyT *` | `ValT *` |

> **Do not modify the container while iterating.** Insertions or deletions during a loop produce undefined behaviour.

---

## 10. Sorting - `csort`

`csort` is a sorting algorithm module. It provides a stable, iterative mergesort.

*Stable* means that two elements that compare as equal always preserve their original relative order after sorting. If Alice and Bob both have score 85 and Alice appeared first in the input, she will still appear first in the sorted output. This matters whenever you sort by one field and want ties to remain in their original sequence.

*Iterative* means the algorithm uses an explicit work buffer instead of function call recursion, so it never causes a stack overflow no matter how large the input is.

In most cases you will reach `csort` indirectly through `cvec_sort` or `cvector_sort_with_comparison_proc`. The lower-level `csort_sort` function is available when you need to sort a plain C array directly.

**Header:** `#include <csort.h>`

### Sorting a Vector

```c
cvec_construct(values, double);
cvec_push_rvalue(values, 3.14);
cvec_push_rvalue(values, 1.41);
cvec_push_rvalue(values, 2.71);

cvec_sort(values);  /* Ascending, using the default double comparator */

cvec_destroy(values);
```

### Sorting a Plain C Array

When sorting a plain array, a getter function is required to abstract element access:

```c
typedef struct { char name[64]; int age; } Person;

int compare_by_age(const void *a, const void *b) {
    const Person *p = (const Person *)a;
    const Person *q = (const Person *)b;
    return (p->age > q->age) - (p->age < q->age);
}

void *person_getter(void *collection, size_t index) {
    return &((Person *)collection)[index];
}

Person people[] = {{"Alice", 30}, {"Bob", 25}, {"Charlie", 35}};

csort_sort(people, 3, sizeof(Person), person_getter, compare_by_age, NULL);
```

`csort_get_default_comparison_proc` (used internally by `cvec_sort`) recognizes every standard integral and floating-point type plus genuine `char*`/`const char*` pointer variables, but not a fixed-size char array field like `Person.name` above; sort a struct containing one with an explicit comparator, as `compare_by_age` does here, rather than relying on the default comparator.

The default `float`, `double`, and `long double` comparators order a NaN value as greater than every non-NaN value, and equal only to another NaN, so a NaN present anywhere in the collection can never disturb the relative order of the other elements.

**Complexity guarantees:** O(n log n) in all cases; O(n) auxiliary space; stable (equal elements preserve their original order); iterative (no recursion, no stack overflow risk for large inputs).

`csort_sort` returns `true` on success (including the trivial cases of a NULL collection, a zero- or one-element length, or an `elem_size` of 0, none of which have anything to do) and `false` if `length` exceeds the library's own maximum element count, a non-NULL final argument does not have all four of `malloc`/`free`/`calloc`/`realloc` populated, its internal temporary merge buffer could not be allocated, or `length * elem_size` would overflow `size_t`, in which case the collection is left completely untouched. `cvec_sort` and `cvector_sort_with_comparison_proc` call `fatal_err()` instead of returning a value, matching every other mutating type-safe macro in `cvector.h`.

`getter_proc` and `comparison_proc` must both be non-NULL whenever `collection` is non-NULL and `length` is 2 or greater; this holds even when `elem_size` is 0, a case where neither would actually be called. Only a NULL `collection` or a `length` of 0 or 1 exempts a call from needing genuine, non-NULL procs. A call that violates this aborts the process rather than returning `false`.

### Real-World Use Case: Sorting a Printer Queue by Priority

A simple print spooler wants urgent documents to print before ordinary ones, while documents of the same priority still print in the order they were sent. The stable sort guarantee means two same-priority documents are always printed in their original order, without needing a separate tiebreaker field:

```c
typedef struct {
    int      priority;    /* higher value = prints sooner */
    uint64_t submitted_at;
    char     filename[128];
} PrintJob;

int cmp_priority_desc(const void *a, const void *b) {
    const PrintJob *ja = (const PrintJob *)a;
    const PrintJob *jb = (const PrintJob *)b;
    /* Descending: higher priority first */
    return (jb->priority > ja->priority) - (jb->priority < ja->priority);
}

void *job_getter(void *collection, size_t index) {
    return &((PrintJob *)collection)[index];
}

void send_to_printer(PrintJob *queue, size_t count) {
    csort_sort(queue, count, sizeof(PrintJob), job_getter, cmp_priority_desc, NULL);

    for (size_t i = 0; i < count; i++)
        printf("printing: %s (priority %d)\n", queue[i].filename, queue[i].priority);
}
```

Because `csort` is a stable sort, two documents submitted at times `t1 < t2` with identical priority are always printed in the order `t1, t2`, regardless of how many times the queue has been re-sorted.

---

## 11. Memory Pools - `cmempool`

A memory pool pre-allocates a large block of memory up front and hands out slices from it on demand. Compared to calling `malloc` for every object, pool allocation is faster (O(1) with no system calls for each request), produces no fragmentation, and makes peak memory usage predictable: the pool has a fixed capacity that cannot grow beyond what you set at creation.

The library provides two pool allocators: a fixed-size pool (`mempool`) for objects of a single size, and a ranged pool (`r_mempool`) for objects across a range of sizes. Both offer optional thread safety and an optional fallback to the system allocator when the pool is exhausted.

**Header:** `#include <cmempool.h>`

### Fixed-Size Pool - `mempool`

A `mempool` holds a fixed number of elements of a fixed size. Allocation returns a slot from an internal free list; deallocation returns it. There is no fragmentation within the pool.

```c
/* Create a pool of 128 elements, each 64 bytes, thread-safe, no malloc fallback */
mempool *pool = mempool_create(128, 64, /*fallback=*/false, /*single_threaded=*/false, NULL, NULL);

void *a = mempool_alloc_entry(pool);    /* Uninitialized */
void *b = mempool_calloc_entry(pool);   /* Zero-initialized */

/* Use entries ... */

mempool_free_entry(a);
mempool_free_entry(b);
mempool_destroy(pool);
```

#### Pre-allocated Buffer (Embedded and Real-Time Contexts)

For contexts where heap allocation must be avoided entirely, a pool can be constructed from a statically declared buffer:

```c
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(static_buf, 128, 64);

mempool *pool = mempool_create_from_preallocated_buffer(
    static_buf, sizeof(static_buf), 64,
    /*fallback=*/false, /*single_threaded=*/true,
    NULL, NULL);

void *slot = mempool_alloc_entry(pool);
/* ... */
mempool_free_entry(slot);
mempool_destroy(pool);  /* The buffer itself is not freed */
```

`DECLARE_PREALLOCATED_MEMPOOL_BUFFER` declares its buffer with the alignment the pool's internal per-element bookkeeping needs, so it can be handed straight to `mempool_create_from_preallocated_buffer()`. A hand-rolled buffer (not declared via the macro) must be aligned to at least `_Alignof(max_align_t)`, or creation fails with an error. `elem_size` smaller than `sizeof(uintptr_t)` is silently rounded up to fit the free-list pointer, the same way `mempool_create()` handles it; a genuine `elem_size` of `0` is rejected, and so is an `elem_count` of `0`. The per-element stride used to lay out the buffer (and, correspondingly, the pool's own heap-allocated buffer when not using a preallocated one) is further rounded up so that every element the pool hands out is correctly aligned, not just the first.

### Ranged Pool - `r_mempool`

A `r_mempool` covers allocation requests across a configurable range of power-of-two sizes. It maintains an internal sub-pool for each size class and selects the smallest fitting class for each request. Requests that exceed the largest class can fall back to the system allocator.

The following table illustrates the structure produced by `r_mempool_create(4, 12, 9, ...)`:

| Element Size | Element Count |
|---|---|
| 2^**`4`** : 16 bytes    | 2^**`9`** : 512 entries |
| 2^5 : 32 bytes    | 2^8 : 256 entries |
| 2^6 : 64 bytes    | 2^7 : 128 entries |
| 2^7 : 128 bytes   | 2^6 : 64 entries  |
| 2^8 : 256 bytes   | 2^5 : 32 entries  |
| 2^9 : 512 bytes   | 2^4 : 16 entries  |
| 2^10 : 1024 bytes | 2^3 : 8 entries   |
| 2^11 : 2048 bytes | 2^2 : 4 entries   |
| 2^**`12`** : 4096 bytes | 2^1 : 2 entries   |

```c
r_mempool *rpool = r_mempool_create(
    4,  /* smallest_size_power_of_two */
    12, /* largest_size_power_of_two */
    9,  /* number_of_smallest_size_elems_power_of_two */
    fallback_at_last_exhaustion,  /* Fall back to malloc when a sub-pool is full */
    /*single_threaded=*/false,
    NULL,   /* Use default allocator */
    NULL);  /* No error string output */

void *small  = r_mempool_alloc_entry(rpool, 20);   /* Served from the 32-byte sub-pool */
void *medium = r_mempool_alloc_entry(rpool, 100);  /* Served from the 128-byte sub-pool */
void *large  = r_mempool_alloc_entry(rpool, 500);  /* Served from the 512-byte sub-pool */

/* Resize in place; the entry is moved to the nearest fitting sub-pool if necessary */
medium = r_mempool_realloc_entry(rpool, medium, 200);

r_mempool_free_entry(small);
r_mempool_free_entry(medium);
r_mempool_free_entry(large);
r_mempool_destroy(rpool);
```

#### Pre-allocated Buffer (Embedded and Real-Time Contexts)

For contexts where heap allocation must be avoided entirely, a ranged pool, too, can be constructed from a statically declared buffer:

```c
DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(rmempool_buf, /* pool buffer name */
    4,  /* smallest_size_power_of_two */
    12, /* largest_size_power_of_two */
    9,  /* number_of_smallest_size_elems_power_of_two */);

r_mempool *pool = r_mempool_create_from_preallocated_buffer(
    rmempool_buf, sizeof(rmempool_buf), 4, 12, 9,
    fallback_at_last_exhaustion, /*single_threaded=*/true,
    NULL, NULL);

void *slot = r_mempool_alloc_entry(pool, 100);
/* ... */
r_mempool_free_entry(slot);
r_mempool_destroy(pool);  /* The buffer itself is not freed */
```

The same alignment requirement applies here: `DECLARE_PREALLOCATED_RMEMPOOL_BUFFER` already declares a suitably aligned buffer (every sub-pool segment inside it stays correctly aligned as a consequence), while a hand-rolled buffer must be aligned to at least `_Alignof(max_align_t)`.

### Real-World Use Case: A Fixed Pool of Bullets for a Game

A simple arcade-style game can spawn dozens of bullets on screen at once, each one living for only a second or two. Allocating and freeing each bullet with `malloc`/`free` works, but a fixed-size pool is faster (an O(1) free-list operation, no heap fragmentation) and bounds peak memory usage up front, which matters during a hectic "bullet hell" moment:

```c
typedef struct {
    float x, y;
    float vx, vy;
    int   lifetime_frames;
} Bullet;

static mempool *bullet_pool;

void game_init(int max_bullets_on_screen) {
    bullet_pool = mempool_create(
        (size_t)max_bullets_on_screen,
        sizeof(Bullet),
        /*fallback=*/false,      /* return NULL instead of calling malloc */
        /*single_threaded=*/true,
        NULL, NULL);
}

Bullet *fire_bullet(float x, float y, float vx, float vy) {
    Bullet *b = mempool_calloc_entry(bullet_pool);   /* zero-initialised */
    if (!b) return NULL;                              /* too many bullets already on screen */
    b->x = x; b->y = y; b->vx = vx; b->vy = vy;
    b->lifetime_frames = 120;
    return b;
}

void bullet_expire(Bullet *b) {
    mempool_free_entry(b);   /* O(1) - returned to the free list */
}
```

Because every slot is the same size as `Bullet`, there is no fragmentation within the pool. Peak memory is fully determined by `max_bullets_on_screen * sizeof(Bullet)` - no surprises even when the screen is full of bullets.

### Driving Other Containers from a Pool

Any container that accepts a `ccol_memmgmt_procs_t *` can be directed to allocate from a pool. See [Section 20](#20-custom-memory-management) for the complete pattern.

---

## 12. Thread Communication - `cthreadcomm`

When two threads need to share data, you need a safe handoff mechanism. Simply reading and writing the same variable from two threads without coordination is a data race; the result is undefined behaviour that can corrupt data or crash unpredictably.

The thread communication module provides three primitives for passing data between threads safely. All three use a zero-copy ownership transfer model: on a successful send, the sender's pointer is set to `NULL` and the receiver becomes the sole owner of the data. This ensures that only one thread holds a reference to any given payload at a time, eliminating an entire class of concurrency bugs.

**Header:** `#include <cthreadcomm.h>`

### The Message Type

```c
typedef struct {
    void   *data;  /* Pointer to heap-allocated payload; NULL is a valid sentinel */
    size_t  size;  /* Size of the payload in bytes */
} c_message_t;
```

### Circular Queue - Bounded, Blocking

`circular_queue` holds a fixed number of messages. A sender blocks when the queue is full; a receiver blocks when it is empty. This backpressure mechanism is the primary tool for rate-limiting producers.

```c
circular_queue *cq = circular_queue_create(16, NULL);

/* Producer */
c_message_t msg = { .data = strdup("task payload"), .size = 13 };
circq_send_zc(cq, &msg);
/* msg.data is now NULL - ownership has been transferred */

/* Consumer */
c_message_t received;
circq_recv_zc(cq, &received);
printf("Received: %s\n", (char *)received.data);
free(received.data);

/* Non-blocking variants */
c_message_t try_msg = { .data = strdup("non-blocking"), .size = 13 };
ccol_retval_t rc = circq_try_send_zc(cq, &try_msg);
if (rc == ccol_container_full) {
    free(try_msg.data);  /* Ownership was not transferred; caller must free */
}

circular_queue_destroy(cq);
```

### Dynamic Queue - Unbounded

`dynamic_queue` uses a linked list and never blocks a sender. It grows without bound as long as memory is available, making it appropriate when the producer must not stall under any circumstances and the consumer is expected to keep up over time.

```c
dynamic_queue *dq = dynamic_queue_create(NULL);

c_message_t msg = { .data = malloc(sizeof(int)), .size = sizeof(int) };
*(int *)msg.data = 42;
dynmq_send_zc(dq, &msg);

c_message_t received;
dynmq_recv_zc(dq, &received);
printf("Value: %d\n", *(int *)received.data);
free(received.data);

dynamic_queue_destroy(dq);
```

### Channel - Bidirectional, Owner-Worker Pattern

A `channel` wraps two circular queues (one in each direction) and routes messages automatically based on the identity of the calling thread. The thread that calls `channel_create` is the owner; all other threads are workers. This removes the need for separate queue handles at the cost of a thread-identity check on each operation.

```c
channel *ch = channel_create(16, NULL);

void *worker(void *arg) {
    channel *ch = (channel *)arg;

    /* Receive a task from the owner */
    c_message_t task;
    chan_recv_zc(ch, &task);
    printf("Worker received: %s\n", (char *)task.data);
    free(task.data);

    /* Send a result back to the owner */
    c_message_t result = { .data = strdup("done"), .size = 5 };
    chan_send_zc(ch, &result);

    return NULL;
}

/* Owner sends a task */
c_message_t task = { .data = strdup("process this"), .size = 13 };
chan_send_zc(ch, &task);

pthread_t t;
pthread_create(&t, NULL, worker, ch);

/* Owner receives the result */
c_message_t result;
chan_recv_zc(ch, &result);
printf("Owner received: %s\n", (char *)result.data);
free(result.data);

pthread_join(t, NULL);
channel_destroy(ch);
```

`fork(2)` is safe with respect to `circular_queue`/`dynamic_queue`/`channel`'s own internal locking: a `cq->mutex`/`dq->mutex` held by some other thread at the instant of the fork is never inherited by the child already locked, as a defense-in-depth measure (see "Compile-Time Configuration" above for the two supported fork(2) patterns this library asks a caller to follow, and why). Unlike `event_loop`/`cthreadpool`, a queue owns no worker thread of its own, so this measure has no thread-liveness caveat to observe on top of it. This protection can be compiled out via `FORK_SAFETY_REQUIRED=0` for a caller that has no need for it.

### Example: Producer-Consumer with Sentinel Termination

```c
circular_queue *queue;

void *producer(void *arg) {
    for (int i = 0; i < 100; i++) {
        int *data = malloc(sizeof(int));
        *data = i;
        c_message_t msg = { .data = data, .size = sizeof(int) };
        circq_send_zc(queue, &msg);
    }
    /* Send a NULL sentinel to signal completion */
    c_message_t sentinel = { .data = NULL, .size = 0 };
    circq_send_zc(queue, &sentinel);
    return NULL;
}

void *consumer(void *arg) {
    for (;;) {
        c_message_t msg;
        circq_recv_zc(queue, &msg);
        if (!msg.data) break;
        printf("Consumed: %d\n", *(int *)msg.data);
        free(msg.data);
    }
    return NULL;
}

int main(void) {
    queue = circular_queue_create(8, NULL);
    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer, NULL);
    pthread_create(&cons, NULL, consumer, NULL);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    circular_queue_destroy(queue);
    return 0;
}
```

### Real-World Use Case: Generating Photo Thumbnails in the Background

A simple photo album application generates a small thumbnail for each photo the user imports. Doing this resizing work on a background thread keeps the main program (and its UI) responsive while the thumbnail is being created. The zero-copy ownership model means the heap-allocated job struct is never duplicated: `chan_send_zc` nulls the sender's pointer on success, and the worker becomes the sole owner:

```c
typedef struct {
    char photo_path[256];
    int  thumb_width;
    int  thumb_height;
    char thumb_path[256];
} ThumbnailJob;

void *thumbnail_worker(void *arg) {
    channel *ch = (channel *)arg;
    for (;;) {
        c_message_t msg;
        chan_recv_zc(ch, &msg);
        if (!msg.data) break;   /* NULL sentinel signals shutdown */

        ThumbnailJob *job = (ThumbnailJob *)msg.data;
        make_thumbnail(job->photo_path, job->thumb_width, job->thumb_height,
                       job->thumb_path);

        c_message_t done = {
            .data = strdup(job->thumb_path),
            .size = strlen(job->thumb_path) + 1
        };
        free(job);
        chan_send_zc(ch, &done);   /* hand the result back to the main thread */
    }
    return NULL;
}

void make_thumbnail_async(channel *ch, const char *photo, int w, int h,
                          const char *thumb_out) {
    ThumbnailJob *job = malloc(sizeof(ThumbnailJob));
    snprintf(job->photo_path, sizeof(job->photo_path), "%s", photo);
    snprintf(job->thumb_path, sizeof(job->thumb_path), "%s", thumb_out);
    job->thumb_width  = w;
    job->thumb_height = h;

    c_message_t msg = { .data = job, .size = sizeof(*job) };
    chan_send_zc(ch, &msg);   /* job is now NULL - the worker owns it */

    c_message_t done;
    chan_recv_zc(ch, &done);
    printf("thumbnail ready: %s\n", (char *)done.data);
    free(done.data);
}
```

The `channel` automatically routes sends and receives based on thread identity: the thread that called `channel_create` is the owner; all other threads are workers. No separate queue handles are required.

### Multiplexed Waiting - `ccol_select`

`ccol_select` blocks until any one of a set of queue or file descriptor sources becomes ready, analogous to POSIX `select(2)` or `poll(2)` but integrated with the queue primitives. `ccol_select_timed` adds a millisecond deadline measured on `CLOCK_MONOTONIC`. `ccol_select` never performs the receive or send itself, for any selectable type: it only reports which selectable is ready, and the caller performs its own explicit `circq_try_recv_zc`/`dynmq_try_recv_zc`/`circq_try_send_zc`/`dynmq_try_send_zc` (for a queue selectable) or `read(2)`/`recv(2)`/`write(2)`/`send(2)` (for an fd selectable) immediately afterward.

```c
circular_queue *q0 = circular_queue_create(8, NULL);
dynamic_queue  *dq  = dynamic_queue_create(NULL);
int pfd[2];
pipe(pfd);

size_t ready_index;

ccol_retval_t rc = ccol_select_va(&ready_index,
    selectable_from_circq(q0,    ccol_select_read),
    selectable_from_dynq(dq,     ccol_select_read),
    selectable_from_fd(pfd[0],   ccol_select_read));

if (rc == ccol_success) {
    c_message_t msg;
    if (ready_index == 2) {
        /* A file descriptor fired; read it directly. */
        char buf[512];
        ssize_t n = read(pfd[0], buf, sizeof(buf));
        (void)n;
    } else if (ready_index == 0) {
        /* q0 fired; claim the message ourselves (zero-copy). */
        if (circq_try_recv_zc(q0, &msg) == ccol_success) free(msg.data);
    } else {
        /* dq fired. */
        if (dynmq_try_recv_zc(dq, &msg) == ccol_success) free(msg.data);
    }
}
```

The timed variant returns `ccol_timed_out` if the deadline expires before any source fires. A timeout of `0` polls without blocking; `-1` blocks indefinitely (equivalent to `ccol_select`):

```c
ccol_retval_t rc = ccol_select_timed_va(&ready_index, /*timeout_ms=*/200,
    selectable_from_circq(q0, ccol_select_read),
    selectable_from_fd(pfd[0], ccol_select_read));

if (rc == ccol_timed_out) {
    /* No source was ready within 200 ms */
}
```

**Key properties:**

- Readiness only, for every selectable type: a queue win means a message is (probably) available to claim via `circq_try_recv_zc`/`dynmq_try_recv_zc`; a file descriptor read win means `read(2)`/`recv(2)` will (probably) return data. Either explicit call may still find nothing if a concurrent consumer/producer won the race first (TOCTOU, the same contract POSIX `select(2)` itself has); the call must be non-blocking and its result checked.
- Queue-only selectable sets use a condition variable path with no `epoll` overhead. Any file descriptor in the set switches the implementation to `epoll(7)` automatically.
- The same fd may appear in more than one selectable within a single call, in any mix of directions, e.g. watching one connected socket for both readability and writability at once; the underlying registrations are combined automatically, and the returned index resolves to whichever one of the sharing selectables actually became ready.

---

### Persistent Event Loop - `event_loop`

`ccol_select` creates a fresh `epoll(7)` instance on every call, waits for exactly one ready selectable, and tears everything down before returning. `event_loop` is the persistent counterpart: one `epoll` instance and one or more background reactor threads, created once and mutated incrementally (`event_loop_add` / `event_loop_modify` / `event_loop_remove`) as fds and queues come and go, dispatching readiness through callbacks for as long as the loop lives. It reuses the exact same `ccol_selectable` type `ccol_select` uses, so `selectable_from_fd`, `selectable_from_circq`, `selectable_from_dynq`, and `selectable_from_chan` all carry over unchanged. Like `ccol_select`, `event_loop` never performs the receive or send itself, for any selectable type: the callback always performs its own explicit `circq_try_recv_zc`/`dynmq_try_recv_zc` or `read(2)`/`recv(2)`.

`event_loop` is an opaque VALUE handle, not a pointer: it must never be cast to/from `void *`, compared via a pointer cast, or treated as an address. Compare it against `EVENT_LOOP_INVALID` (or use a truthiness check; `EVENT_LOOP_INVALID` is `0`, so `if (!loop)` works as expected). Internally, every use of an `event_loop` is resolved through a library-owned slot table before the underlying reactor object is touched, so a stale handle (one whose loop has already been destroyed) is always detected rather than silently dereferencing freed memory; passing an already-destroyed or otherwise stale handle to `event_loop_destroy` specifically is a fatal error (`abort()`/`SIGABRT`), covering both a purely sequential double-destroy and a concurrent one, rather than risking a double-free. Calling `event_loop_destroy` on a loop from within a callback currently dispatching on one of that loop's own threads (the poller thread, or a dispatch worker) is a fatal error too: that thread is the one destruction would otherwise need to join, and freeing live registrations and the loop itself out from under the still-running callback would be worse than the deadlock this same misuse would cause against `event_loop_shutdown` directly. Defer destruction to another thread, or to after the callback returns, instead.

The fd/registration registry is lock-striped: `num_lock_stripes` independent (mutex, chmap) pairs, each guarding a disjoint subset of registrations (one real fd, or one queue/channel registration's private bridge fd, is always handled by exactly one stripe). `1` means a single shared lock; passing a larger value lets `event_loop_add` / `event_loop_remove` / `event_loop_modify` calls for different fds/registrations proceed concurrently under high-churn multi-threaded use instead of serializing through one lock, at the cost of `num_lock_stripes` mutexes and chmaps allocated up front. Most callers should just pass `1`.

`num_reactor_threads` (a separate constructor parameter from `num_lock_stripes`) is the total OS thread count devoted to this loop's own polling and dispatch. Exactly ONE dedicated thread ever calls `epoll_wait(2)`, regardless of how large `num_reactor_threads` is (this avoids a kernel-level thundering herd: `epoll`'s level-triggered semantics would otherwise wake every thread blocked on the same instance for a single ready event). With `num_reactor_threads == 1`, that one thread also runs every callback inline. With a larger value, that same one polling thread is joined by `num_reactor_threads - 1` separate dispatch worker threads that actually execute callbacks, so total thread count for a given `num_reactor_threads` is always exactly that value in both configurations. A single registration's callback is never invoked concurrently with itself, and a read registration and a write registration sharing the same fd are never invoked concurrently with each other either (stricter than "no self-concurrency" alone, so a callback pair sharing state across both directions of one fd, e.g. one TLS connection object, needs no locking of its own on that account); a second dispatch for the same registration is also never collected while an earlier one is still queued or executing, so application code calling `event_loop_modify` from within an in-flight callback (a supported, commonly used pattern) never races a concurrently-collected second dispatch for that same registration.

`event_reg` (the handle `event_loop_add` returns, and the type every other `event_loop_*` registration function takes) is, like `event_loop` itself, an opaque VALUE handle, not a pointer: it must never be cast to/from `void *`, compared via a pointer cast, or treated as an address. Compare it against `EVENT_REG_INVALID` (or use a truthiness check; `EVENT_REG_INVALID` is `0`). Every use of an `event_reg` is resolved through its own loop's registration table before anything is dereferenced, so a stale or already-removed handle is always detected and rejected cleanly (`ccol_invalid_args`, or generation `0`) rather than risking a use-after-free, even when two threads race each other calling `event_loop_modify`/`_pause`/`_resume`/`_remove`/`event_loop_reg_generation` on the very same registration.

`event_loop_reg_generation(loop, reg)` returns a monotonically increasing, loop-wide-unique identity token minted once per fd when it is first registered (shared by both directions on the same fd, and preserved across `event_loop_modify`), for a caller's own defensive bookkeeping across fd reuse (e.g. detecting that an fd number has been closed and reused by an unrelated connection since a caller last read it). It is not required for basic correctness: dispatch already validates a registration's liveness before invoking any callback unconditionally, so a stale, already-fetched batch entry for an already-removed (or fd-reused) registration is always a safe no-op regardless of whether a caller ever inspects the generation itself.

`max_events_per_wait` bounds how many ready events a single `epoll_wait(2)` call drains, not the number of registrations the loop can hold; it must be between 1 and `INT_MAX` inclusive, and small enough that `max_events_per_wait * sizeof(struct epoll_event)` does not overflow `size_t`, since the value is narrowed to `epoll_wait(2)`'s own `int` parameter and used to size the poller thread's own events buffer. `event_loop_create`/`event_loop_create_with_mprocs` reject a value outside that range with a `NULL` handle.

```c
event_loop_construct(loop, /*max_events_per_wait=*/32, /*num_lock_stripes=*/1,
                      /*num_reactor_threads=*/1);

circular_queue *jobs = circular_queue_create(64, NULL);

void on_job_ready(event_loop loop, ccol_selectable *sel, void *arg) {
    c_message_t msg;
    if (circq_try_recv_zc(sel->cq, &msg) != ccol_success) return; /* lost the race */
    printf("job: %s\n", (char *)msg.data);
    free(msg.data);
}

event_handlers_t handlers = { .on_readable = on_job_ready };
event_reg reg = event_loop_add(loop, selectable_from_circq(jobs, ccol_select_read),
                                handlers, NULL, NULL);

c_message_t msg = { .data = strdup("build #42"), .size = 10 };
circq_send_zc(jobs, &msg);   /* on_job_ready fires asynchronously, on the reactor thread */

/* event_loop_shutdown blocks until the reactor thread is joined, so no
   dispatch can still be touching jobs/reg by the time it returns; tearing
   these down immediately after circq_send_zc, with no such synchronization,
   would race the callback that hasn't necessarily run yet. */
event_loop_shutdown(loop);
event_loop_remove(loop, reg);
circular_queue_destroy(jobs);
event_loop_destroy(loop);
```

A minimal single-connection echo handler over a raw socket shows the fd side:

```c
typedef struct { event_loop loop; event_reg reg; } conn_ctx_t;

void close_conn(conn_ctx_t *ctx, int fd) {
    /* Self-removal from within the callback that triggered it is safe */
    event_loop_remove(ctx->loop, ctx->reg);
    close(fd);
    free(ctx);
}

void on_client_readable(event_loop loop, ccol_selectable *sel, void *arg) {
    conn_ctx_t *ctx = (conn_ctx_t *)arg;
    char buf[512];
    ssize_t n = read(sel->fd, buf, sizeof(buf));
    if (n <= 0) { close_conn(ctx, sel->fd); return; }  /* EOF or error */
    write(sel->fd, buf, (size_t)n);  /* echo back */
}

void on_client_error(event_loop loop, ccol_selectable *sel, void *arg) {
    close_conn((conn_ctx_t *)arg, sel->fd);
}

/* conn_ctx_t is allocated once the connection is accepted, e.g. from a
   listen-socket's own on_readable handler after accept(2): */
conn_ctx_t *ctx = malloc(sizeof(*ctx));
ctx->loop = loop;
event_handlers_t client_handlers = { .on_readable = on_client_readable,
                                      .on_error = on_client_error };
ctx->reg = event_loop_add(loop, selectable_from_fd(client_fd, ccol_select_read),
                           client_handlers, ctx, NULL);
```

Both directions may be registered on the same fd at once (e.g. a full-duplex socket being read and written concurrently) by calling `event_loop_add` twice, once per direction; each call returns an independent `event_reg`. Flipping a single registration's direction over time instead (e.g. a non-blocking connect: write-interest until the connect completes, then read-interest afterward) uses one registration plus `event_loop_modify`.

A queue or channel selectable has no such one-registration-per-direction limit: any number of `event_loop_add` calls for the same queue+direction (across one or more `event_loop` instances), and any mix of those with a concurrent `ccol_select()` call on the same queue+direction, are all live at once, and every one of them eventually gets a turn rather than the earliest-registered ones being starved by a later one. A message that arrives while more than one such listener is registered wakes exactly one of them at a time (never all of them, to avoid a thundering herd); whichever one is woken checks, once its own callback returns, whether the queue is still ready for its direction and, if so, hands the wake to the next listener in line. A backlog deeper than the number of live listeners on that queue+direction may still need a further, unrelated send/receive to fully drain, the same "a notification is not guaranteed to correspond to exactly one message" characteristic a single listener already has (a callback that must not leave messages stranded under bursty traffic should call `circq_try_recv_zc()`/`dynmq_try_recv_zc()` in a loop until it returns `ccol_container_empty`); what's guaranteed is that no live listener is ever passed over indefinitely.

For a caller pattern where an fd registration needs to temporarily stop receiving events and later come back (e.g. a connection handed off to a worker thread for blocking body I/O, then handed back to the reactor for its next request), `event_loop_pause`/`event_loop_resume` are far cheaper than an `event_loop_remove` immediately followed by a later `event_loop_add`: the registration stays fully intact (no heap allocation/free, no fd-registry chmap churn) and only the fd's combined epoll interest mask is recomputed to exclude/include it. If every direction currently registered on the fd ends up paused, the fd is removed from the kernel's own epoll interest set entirely rather than merely narrowed to an empty mask, so it produces zero further wakeups of any kind while paused, including on its own error/hangup condition (which the kernel would otherwise keep reporting regardless of the requested interest mask):

```c
event_loop_pause(loop, reg);    /* no more callbacks for reg until resumed */
/* ... a worker thread does its own blocking I/O on the fd directly ... */
event_loop_resume(loop, reg);   /* interest restored; same reg, same generation */
```

**Key properties:**

- `num_reactor_threads` total background threads per `event_loop` (one dedicated polling thread, plus `num_reactor_threads - 1` dispatch worker threads when greater than 1), spawned at creation and all joined at `event_loop_shutdown` / `event_loop_destroy`; multiple independent instances share no global state.
- A single registration's callback is never invoked concurrently with itself, and a read and a write registration sharing the same fd are never invoked concurrently with each other, regardless of how many reactor threads are configured.
- `event_loop_reg_generation` gives every fd registration a loop-wide-unique, monotonically increasing identity token, stable across `event_loop_modify` and shared by both directions on the same fd, for detecting fd reuse from application code; dispatch itself already validates a registration's liveness unconditionally, so this is for the caller's own bookkeeping, not required for internal correctness.
- `event_loop_modify`/`_pause`/`_resume`/`_remove`/`event_loop_reg_generation` may all be called concurrently, from different threads, against the very same `event_reg`; the losing side of a race against a concurrent `event_loop_remove` always sees the registration as already removed, never a use-after-free.
- `event_loop_pause`/`event_loop_resume` are fd-only (same restriction as `event_loop_modify`) and never change `event_loop_reg_count` or `event_loop_reg_generation`; pausing an already-paused registration, or resuming one that isn't paused, is a no-op success.
- `event_loop_remove` is safe to call from within a registration's own callback (self-removal on error is a common pattern) as well as from any other thread, including concurrently with an in-flight dispatch for the same registration.
- Removing an fd registration or destroying the loop never closes the fd itself, and never destroys a registered queue; ownership stays exactly where `selectable_from_fd`/`selectable_from_circq`/etc. already put it.
- A queue registered via `selectable_from_circq`/`selectable_from_dynq`/`selectable_from_chan` must outlive its registration: `circular_queue_destroy`/`dynamic_queue_destroy`/`channel_destroy` assert if the queue still has a live `event_loop` registration or an in-progress `ccol_select`/`ccol_select_timed` call watching it, since the still-linked waiter would otherwise reference the queue's own, about-to-be-freed mutex. Call `event_loop_remove` (or let every watching `ccol_select`/`ccol_select_timed` call return) before destroying the queue; destroying the loop first is fine, since `event_loop_destroy`/`event_loop_remove` always unlink a queue's waiter before it is ever touched again.
- The supported way to combine `fork(2)` with `event_loop` is one of the two patterns described under "Compile-Time Configuration" in section 4: `fork(2)` before creating a loop, or create one and `fork(2)` immediately followed by `exec(3)`. As a defense-in-depth measure on top of that, a fresh `event_loop_create`/`_destroy`/`_add`/`_remove`/`_modify`/`_pause`/`_resume` call, from any thread, in either the parent or a freshly forked child, never hangs waiting on a lock that some other (possibly no-longer-existing, since `fork()` duplicates only the calling thread) thread happened to hold at the instant of the fork. A loop that was already live across the fork is a different matter regardless of that measure: `fork()` does not duplicate its poller/dispatch threads, so an inherited loop can no longer dispatch anything in the child; treat such a loop as inert there (safe to `event_loop_destroy`, not usable for further dispatch) rather than keep registering against it. This protection can be compiled out via `FORK_SAFETY_REQUIRED=0` for a caller that has no need for it.

---

## 13. LRU Cache - `clrucache`

A cache stores the results of expensive operations so that repeated requests for the same input return immediately without redoing the work. An LRU (Least-Recently-Used) cache has a fixed capacity; when it is full and a new entry needs to be added, the entry that has gone the longest without being accessed is evicted first. This keeps frequently requested results in memory and lets old, rarely used ones fall out automatically.

`clrucache` is a thread-safe LRU cache backed by a hash map for O(1) lookup and a doubly-linked list for O(1) eviction. It supports optional remote getter and setter callbacks to integrate transparently with an external backing store such as a database.

`clru_cache` is an opaque VALUE handle, not a pointer: it must never be cast to/from `void *`, compared via a pointer cast, or treated as an address. Compare it against `CLRU_CACHE_INVALID` (or use a truthiness check; `CLRU_CACHE_INVALID` is `0`, so `if (!cache)` works as expected). Internally, every use of a `clru_cache` is resolved through a library-owned slot table before the underlying cache object is touched, so a stale handle (one whose cache has already been destroyed) is always detected rather than silently dereferencing freed memory; passing an already-destroyed or otherwise stale handle to `clru_destroy`/`__clrucache_destroy` specifically is a fatal error (`abort()`/`SIGABRT`), covering both a purely sequential double-destroy and a concurrent one, rather than risking a double-free.

**Header:** `#include <clrucache.h>`

### Concurrency Guarantees

All operations are serialised via a single global mutex combined with per-entry condition variables. The key properties are:

- Multiple threads requesting the same uncached key coalesce: exactly one remote fetch executes; all others block and receive the same result when it completes.
- A getter for a key that is currently being set blocks until the set completes, so it always reads a consistent value.
- Multiple setters for the same key are serialised.

The eviction callback is invoked while the cache mutex is held. It **must not** call back into the cache.

### Basic Usage

```c
/* Cache mapping int keys to double values, capacity 128 */
clru_construct(cache, int, double, 128, NULL, NULL, NULL);

/* Store a value */
int k = 42;
double v = 3.14;
clru_set(cache, k, v);

/* Retrieve a value - val_ptr is a pointer to the value type, not cmap_pair */
double out = 0.0;
if (clru_get(cache, k, &out) == ccol_success) {
    printf("%.2f\n", out);
}

clru_destroy(cache);
```

### Remote Getter - Read-Through

A remote getter is called on a cache miss. The cache takes ownership of the heap-allocated value returned by the getter and frees it with the cache's own allocator (its custom allocator if one was provided at construction, or `free()` otherwise); the getter's allocation must therefore be made with that same allocator (its custom `malloc`/`calloc` if one was provided, or plain `malloc()` otherwise), never a different, unrelated allocator. Concurrent requests for the same missing key coalesce: only one fetch executes, and all waiters receive the result.

```c
bool load_from_db(const cmap_pair *key, cmap_pair *val) {
    double *result = malloc(sizeof(double));
    if (!result) return false;
    *result = /* ... query database ... */;
    val->ptr  = result;
    val->size = sizeof(double);
    return true;
}

clru_construct(cache, int, double, 256, load_from_db, NULL, NULL);

int k = 7;
double val = 0.0;
/* On a miss, load_from_db is called exactly once regardless of concurrent threads */
if (clru_get(cache, k, &val) == ccol_success) {
    printf("%.2f\n", val);
}

clru_destroy(cache);
```

### Remote Setter - Write-Through

A remote setter is called synchronously before the cache is updated. If the remote call fails, the cache is not updated and `clru_set` returns `ccol_unexpected_failure`.

```c
bool write_to_db(const cmap_pair *key, const cmap_pair *val) {
    return /* write key/value to external store */;
}

clru_construct(cache, int, double, 256, NULL, write_to_db, NULL);

int k = 7;
double v = 2.71;
if (clru_set(cache, k, v) == ccol_unexpected_failure) {
    /* remote write failed; cache is unchanged */
}

clru_destroy(cache);
```

### Eviction Callback

```c
void on_evict(const cmap_pair *key, const cmap_pair *val) {
    printf("evicted key=%d\n", *(const int *)key->ptr);
    /* Must NOT call back into the cache - mutex is held */
}

clru_construct(cache, int, double, 4, NULL, NULL, on_evict);
/* When the 5th unique key is inserted, the LRU entry is evicted */
clru_destroy(cache);
```

### Real-World Use Case: Caching Dictionary Word Lookups

A simple dictionary application looks up word definitions from a large word list on disk. Reading the file and scanning for a match takes a noticeable moment; looking up a word that was already searched for recently should feel instant. The remote getter coalesces concurrent misses for the same word, so even if several parts of the program ask for the same not-yet-cached word at the same time, only one slow lookup actually happens:

```c
/* Called automatically by the cache on a miss.
   Looks up a word's definition from the (slow) dictionary file. */
bool load_definition(const cmap_pair *key_pair, cmap_pair *val_pair) {
    const char *word = (const char *)key_pair->ptr;

    char *definition = slow_dictionary_lookup(word);   /* e.g. scans a large file */
    if (!definition) return false;                       /* word not found */

    val_pair->ptr  = definition;
    val_pair->size = strlen(definition) + 1;
    return true;
}

/* Module-level cache: remember the 500 most recently looked-up words */
clru_construct(dictionary_cache, char*, char*, 500, load_definition, NULL, NULL);

/* Called every time the user looks up a word */
void lookup_word(const char *word) {
    clru_redeclare(dictionary_cache, char*, char*);

    char *w = (char *)word;
    char *definition = NULL;
    if (clru_get(dictionary_cache, w, &definition) != ccol_success) {
        printf("No definition found for \"%s\".\n", word);
        return;
    }

    printf("%s: %s\n", word, definition);
    free(definition);   /* clru_get transferred ownership of this string to us */
}
```

The LRU eviction policy bounds memory usage: the 500 most recently looked-up words stay hot in memory; older ones are quietly forgotten. The coalescing property means that looking up the same not-yet-cached word from several places at once causes exactly one slow file scan rather than several redundant ones.

### Memory Ownership for Retrieved Values

`clru_get` memory behavior depends on the value type. Only `char *` values cause a heap allocation; for all other types no heap allocation occurs.

**Non-`char *` value types** - the macro converts the stored value to `*val_ptr`'s own type the same way a plain C assignment would, with no heap allocation. The caller receives the value in a plain typed variable; `free()` is neither needed nor valid:

```c
clru_construct(cache, int, double, 128, NULL, NULL, NULL);

int k = 42;
double v = 3.14;
clru_set(cache, k, v);

double out = 0.0;
if (clru_get(cache, k, &out) == ccol_success) {
    printf("%.2f\n", out);
    /* No free() - no heap allocation occurred */
}

clru_destroy(cache);
```

**`char *` value types** - the macro transfers ownership of the heap-allocated string to the caller via `*(char **)val_ptr`. The caller **must** free it when done. The allocation uses the cache's custom allocator if one was provided at construction, or `malloc()` otherwise:

```c
clru_construct(str_cache, int, char *, 64, NULL, NULL, NULL);

int k = 1;
char *greeting = "hello";
clru_set(str_cache, k, greeting);

char *s = NULL;
if (clru_get(str_cache, k, &s) == ccol_success) {
    printf("%s\n", s);
    free(s);   /* Required - clru_get transferred heap ownership to the caller */
               /* Use custom_free(s) instead if a custom allocator was provided */
}

clru_destroy(str_cache);
```

Each call to `clru_get` produces an independent heap allocation for the string. Two successive calls to `clru_get` for the same key return two independent pointers that must each be freed separately.

The same allocator rule applies to `clrucache_get_full` for all value types: `val_out->ptr` is allocated with the cache's custom allocator (or `malloc()` if none was configured) and must be freed with the matching function.

### Cross-Scope Usage

Pass the cache handle across function boundaries and use `clru_redeclare` to restore type information. This is required before calling `clru_get` or `clru_set`; both macros rely on the companion type variables to determine the value type. Note that `clru_redeclare` requires a simple local identifier, not a struct-member expression like `ga->cache`; declare a local alias first if necessary.

```c
void read_and_write(clru_cache c) {
    clru_redeclare(c, int, double);
    int k = 1;
    double v = 1.0;
    clru_set(c, k, v);

    double out = 0.0;
    clru_get(c, k, &out);
}

int main(void) {
    clru_construct(cache, int, double, 64, NULL, NULL, NULL);
    read_and_write(cache);
    clru_destroy(cache);
    return 0;
}
```

### Scoped Variant

```c
void process(void) {
    clru_construct_scoped(cache, int, double, 64, NULL, NULL, NULL);
    /* cache is destroyed automatically when the function returns */
}
```

### Reference: Core Operations

**Lifecycle**

| Macro / Function | Description |
|---|---|
| `clru_declare(name, KeyT, ValT)` | Declare the cache variable and companion type variables without initialising |
| `clru_declare_scoped(name, KeyT, ValT)` | Declare with auto-cleanup via `__attribute__((cleanup(...)))`, without initialising |
| `clru_redeclare(name, KeyT, ValT)` | Restore type information in a new scope after passing the cache across a function boundary |
| `clru_init(name, capacity, getter, setter, evict_cb)` | Initialise a previously declared cache; calls `fatal_err()` on failure |
| `clru_construct(name, KeyT, ValT, capacity, getter, setter, evict_cb)` | Declare and initialise in one step |
| `clru_construct_scoped(name, KeyT, ValT, capacity, getter, setter, evict_cb)` | Declare, initialise, and register auto-cleanup |
| `clru_destroy(name)` | Destroy the cache and set the handle to `CLRU_CACHE_INVALID`; fatal on an already-destroyed/stale handle |
| `clrucache_size(cache)` | Return the number of live entries currently stored |
| `clrucache_capacity(cache)` | Return the configured capacity |

**Get / Set**

| Macro / Function | Description |
|---|---|
| `clru_get(name, key, val_ptr)` | Retrieve the value for `key`; `key` is converted to `KeyT` the same way a plain C assignment would. `val_ptr` is a pointer to the value type, **not** `cmap_pair *`. For non-`char *` val types, the stored value is converted to `*val_ptr`'s own type the same way a plain C assignment would (not a raw byte copy); no heap allocation occurs. For `char *` val types, `*(char **)val_ptr` is set to a heap-allocated string allocated by the cache's custom allocator (or `malloc()` if none was configured); the caller must free it with the matching function. Returns `ccol_success`, `ccol_key_not_found`, or another error code. |
| `clru_set(name, key, val)` | Store `val` for `key`; `key` and `val` are converted to `KeyT`/`ValT` the same way a plain C assignment would; if a remote setter was provided it is called first; returns `ccol_success` or `ccol_unexpected_failure` on remote failure |

---

## 14. Structured Logger - `clogger`

Logging is how a running program records what it is doing and what went wrong. `clogger` writes structured log lines: instead of free-form text, every message is a sequence of `key=value` pairs that can be filtered, searched, and aggregated programmatically. Three output formats are supported: logfmt (default, plain text, human-readable), NDJSON (one JSON object per line, easy to parse with tools like `jq`), and RFC 5424 syslog (for integration with system logging infrastructure). All writes are serialised through a mutex, making the logger safe to call from multiple threads without any extra coordination, for as long as every handle involved stays open. Closing a handle (`clog_close`) is the one operation this does not cover: as with every other handle-based type in this library, it is the caller's responsibility to ensure no other thread is still using (or concurrently closing) that same handle when it is closed.

**Header:** `#include <clogger.h>`

A `clog` handle is an opaque value (not a pointer; never cast it to or from `void *`), returned by `clog_open_fd_mp`/`clog_open_file_mp`, or `CLOG_INVALID` on failure. Every function documented below expects a live logger handle; passing `CLOG_INVALID`, or a handle whose logger has already been closed, terminates the program rather than silently doing nothing.

The supported way to combine `fork(2)` with `clogger` is one of the two patterns described under "Compile-Time Configuration" in section 4: `fork(2)` before opening a logger, or open one and `fork(2)` immediately followed by `exec(3)`. As a defense-in-depth measure on top of that, a logger handle open at the time of a `fork()` not followed by `exec(3)` remains technically usable in the child: logging, deriving, and closing it all continue to work correctly on both sides, with no risk of a lock inherited mid-operation leaving the child permanently stuck. This protection can be compiled out via `FORK_SAFETY_REQUIRED=0` for a caller that has no need for it.

### Output Formats

Three formats are supported, selectable at any time via `clog_set_format`. The default is logfmt.

#### Logfmt (default)

Every log line follows the pattern:

```
ts=<ISO-8601-UTC> level=<L> proc=<name>(<pid>):<tname>(<tid>) src=<file>:<line> func=<fn> [fields] msg=<text>
```

`proc` identifies the executable basename and main PID in the first group, and the OS thread name and thread ID in the second. `log_error`, `log_alert`, and `log_fatal` append a backtrace as tab-indented continuation lines that do not start with `ts=`, allowing log aggregators to group them with their parent record:

```
ts=2026-05-29T21:52:39.096473Z level=ERROR proc=myapp(1234):main(1234) src=main.c:42 func=handle env=prod msg="db timeout"
	#0 ./myapp(handle+0x1a) [0x7f...]
	#1 ./myapp(main+0x42) [0x7f...]
```

If the backtrace itself cannot be captured (see the note on `-rdynamic`/`execinfo.h` below), or none of its frame lines fit within the internal buffer cap described below (each frame that is too long to fit is skipped independently in favor of any shorter frame that follows, rather than discarding the whole backtrace the moment one frame does not fit), a single `\t#error backtrace unavailable` continuation line is emitted in place of the missing per-frame lines, so a reader can always tell a backtrace was requested but omitted apart from one never having been requested at all.

#### JSON (NDJSON)

When `CLOG_FMT_JSON` is selected, each log record is a single self-contained JSON object followed by a newline:

```json
{"ts":"2026-05-29T21:52:39.096473Z","level":"INFO","proc":"myapp(1234):main(1234)","src":"main.c:9","func":"main","env":"prod","msg":"starting up"}
{"ts":"2026-05-29T21:52:39.096642Z","level":"ERROR","proc":"myapp(1234):main(1234)","src":"main.c:10","func":"main","env":"prod","msg":"db failed","bt":["#0 main+0x16d","#1 libc.so.6+0x29ca8"]}
```

Structured fields appear as top-level JSON keys (insertion order). For `log_error`, `log_alert`, and `log_fatal` the backtrace is embedded as a `"bt"` string array inside the same JSON object instead of being written as separate continuation lines. In the rare case that the backtrace itself cannot be embedded (a transient allocation failure, or the record is already near the internal buffer cap described below), the record is still closed normally with a `"bt_error":"unavailable"` key in place of `"bt"`, so a missing backtrace is always visible rather than silently absent from an otherwise ordinary-looking record.

Every string value (the message, and every field key/value) is validated as UTF-8 before being embedded, since RFC 8259 requires JSON text to be valid Unicode. A well-formed multi-byte UTF-8 sequence is passed through unescaped; a byte or byte sequence that is not well-formed (a lone continuation byte, an overlong or otherwise out-of-range lead byte, an encoded UTF-16 surrogate half, or a multi-byte sequence truncated by the end of the string) is replaced, one invalid byte at a time, with the Unicode replacement character (U+FFFD). This means a message or field value built from arbitrary or binary data can never produce a JSON record with invalid Unicode content, even though `clogger`'s own API takes plain `char *` strings with no encoding of their own to declare.

#### Syslog (RFC 5424)

When `CLOG_FMT_SYSLOG` is selected, each record is emitted as a single RFC 5424 message:

```
<PRI>1 TIMESTAMP HOSTNAME APP-NAME PID MSGID [ccol proc="name(pid):tname(tid)" src="file:N" func="fn" [fields]] MSG
```

The `PRI` field encodes both the facility (default `CLOG_SYSLOG_USER`; see `clog_set_facility`) and the severity level mapped from `clog_level_t` according to RFC 5424 (TRACE/DEBUG -> 7, INFO -> 6, WARN -> 4, ERROR -> 3, ALERT -> 1, FATAL -> 0). For `log_error`, `log_alert`, and `log_fatal` each backtrace frame is emitted as a separate syslog message carrying the same PRI, TIMESTAMP, and MSGID as the record it continues, with its own frame text backslash-escaped the same way the message text is (see below); the shared TIMESTAMP is what lets a reader correlate a record with its own backtrace frames rather than whichever frame a log collector happened to receive around the same time. If the backtrace itself cannot be captured, or none of its frames fit in a single syslog message, a single additional syslog message carrying the text `#error backtrace unavailable` is emitted in its place, the same visible-omission guarantee logfmt and JSON each provide in their own way.

Control characters (including a literal newline or carriage return) in the message text, in every structured-data value, and in each backtrace frame's own text are backslash-escaped (`\n`, `\r`, `\t`, or `\xNN`), the same way logfmt and JSON already escape them in their own message/field output. `HOSTNAME` and `APP-NAME` are filtered to RFC 5424 PRINTUSASCII (`0x21`-`0x7e`) when the logger is created, dropping any other byte rather than truncating at it, so neither field can contain a byte needing this same escaping in the first place; either falls back to `-` if filtering leaves no bytes at all. Together this guarantees a single syslog record can never be split across multiple lines by its own content.

RFC 5424 caps an SD-PARAM-NAME at 32 characters. A field key longer than that is shortened to its first 23 characters followed by `~` and an 8-hex-digit hash of the full key (32 characters total), rather than a bare 32-character prefix; two distinct long keys that happen to share the same 32-character prefix therefore still get distinct SD-PARAM-NAMEs.

`CLOG_FMT_SYSLOG` is restricted to **fd-based loggers** (`clog_open_fd` / `clog_open_fd_mp`). Calling `clog_set_format` with `CLOG_FMT_SYSLOG` on a file-backed logger is a silent no-op. The fd must be connected to a syslog daemon beforehand; on Linux this is typically a `SOCK_DGRAM` Unix socket at `/dev/log`:

```c
int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
struct sockaddr_un sa = { .sun_family = AF_UNIX };
strncpy(sa.sun_path, "/dev/log", sizeof sa.sun_path - 1);
connect(fd, (struct sockaddr *)&sa, sizeof sa);

clog lg = clog_open_fd(fd, CLOG_INFO, NULL);
clog_set_format(lg, CLOG_FMT_SYSLOG);
clog_set_facility(lg, CLOG_SYSLOG_DAEMON);

log_info(lg, "service started");
clog_close(lg);
close(fd);
```

Keep individual messages under 2 KiB to stay within typical syslogd datagram limits. Log rotation is unavailable for fd-based loggers.

Because an fd-based logger's target may be a pipe or a socket (as in the example above), the process ignores `SIGPIPE` from the moment the first logger of any kind is created; if the peer end ever goes away, the next write is dropped silently (matching every other unrecoverable write error) instead of terminating the process.

An fd-based logger's own fd may be blocking or non-blocking. If the fd is non-blocking and momentarily cannot accept more data (`EAGAIN`/`EWOULDBLOCK`, e.g. a UDP socket's send buffer is full under load), the logging call waits for the fd to become writable again and retries rather than dropping the rest of the record; only a genuinely unrecoverable write error (or a peer that has gone away, per the `SIGPIPE` note above) drops a record.

---

The format is stored on the **shared backing store**, so it applies to all logger handles that write to the same file descriptor (root and every derived logger). Setting it via any handle takes effect immediately for all of them:

```c
clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, NULL, NULL, NULL);
clog_set_format(lg, CLOG_FMT_JSON);

clog derived = clog_derive(lg);
/* derived also writes JSON - it shares the same backing store */

clog_format_t fmt = clog_get_format(lg); /* CLOG_FMT_JSON */
```

Backtrace capture requires linking the binary with `-rdynamic` and is available on glibc, macOS, and FreeBSD (any platform providing `execinfo.h`); elsewhere, or on a transient allocation failure while resolving symbol names, capture itself fails, but the omission is never silent; each format surfaces it visibly in its own way, as described above.

Each logger's write buffer grows as needed up to an internal 16 MiB hard cap. In the rare case that a single record (an oversized message, or a very large accumulation of structured fields) would exceed that cap, the record is not truncated or emitted malformed: the oversized content is discarded and a short, well-formed placeholder record is written in its place instead, in the same output format, noting the original message's size in bytes. The same placeholder mechanism also covers a transient, size-unrelated allocation failure (e.g. while enumerating structured fields); in that case the placeholder's note reports the allocation failure directly rather than misattributing it to the record's size. A message longer than 1023 bytes that cannot even be formatted in full under sustained memory pressure is truncated with a visible `...[truncated]` marker rather than silently emitted as if it were complete. Even in the extreme case where memory pressure is severe enough that the placeholder record itself cannot be built, a minimal diagnostic line is written directly in its place, so a record can never be silently reduced to zero written bytes with no trace of it anywhere in the output.

### Basic Usage

```c
#include <clogger.h>

/* Logger targeting stderr */
clog lg = clog_open_fd_mp(2, CLOG_INFO, NULL, NULL);

/* Attach persistent fields */
clog_set_field(lg, "env",     "prod");
clog_set_field(lg, "service", "auth");

log_info(lg,  "starting up");
log_warn(lg,  "config missing: %s", "timeout");
log_error(lg, "db failed: %s", "timeout");   /* also appends a backtrace; log_alert does too */
/* log_fatal appends a backtrace AND terminates the process via exit(EXIT_FAILURE) */

clog_close(lg);
```

Sample output:

```
ts=2026-05-29T21:52:39.096473Z level=INFO  proc=myapp(1234):main(1234) src=main.c:9  func=main env=prod service=auth msg="starting up"
ts=2026-05-29T21:52:39.096512Z level=WARN  proc=myapp(1234):main(1234) src=main.c:10 func=main env=prod service=auth msg="config missing: timeout"
ts=2026-05-29T21:52:39.096642Z level=ERROR proc=myapp(1234):main(1234) src=main.c:11 func=main env=prod service=auth msg="db failed: timeout"
	#0 ./myapp(main+0x16d) [0x563b...]
	#1 /lib/x86_64-linux-gnu/libc.so.6(+0x29ca8) [0x7f78...]
```

### File-Backed Logger with Rotation

Log rotation is available only for file-backed loggers. Both size-based and time-based rotation can be enabled independently:

```c
clog_rotation_cfg_t cfg = {
    .size_rotation_enabled  = true,
    .max_file_size          = 50L * 1024L * 1024L,  /* 50 MiB */
    .time_rotation_enabled  = true,
    .rotation_interval_secs = 86400,                 /* 24 hours */
    .max_rotated_files      = 7,                     /* keep one week of logs */
};

clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, &cfg, NULL, NULL);
```

When a rotation fires the current file is renamed to `<path>.<YYYYMMDDHHMMSS>` (e.g. `app.log.20260529215239`) and a new file is opened. Collisions within the same second are resolved with a zero-padded `_0001`, `_0002`, ... suffix. The oldest rotated files beyond `max_rotated_files` are deleted automatically; a zero or negative `max_rotated_files` falls back to `CLOG_DEFAULT_MAX_ROTATED_FILES` (7) rather than disabling pruning, so a logger configured with rotation enabled never accumulates an unbounded number of rotated files on disk. Only files matching this exact `<path>.<YYYYMMDDHHMMSS>[_NNNN][.gz]` pattern are ever considered for deletion or compression, so an unrelated file in the same directory (even one that starts with a similarly-formatted timestamp) is never touched.

Pass `NULL` as the configuration to open a file-backed logger without rotation.

### Custom Allocator

Both creation functions accept a `ccol_memmgmt_procs_t *mprocs` as the last parameter. Pass `NULL` to use the default `malloc`/`calloc`/`realloc`/`free`. Passing a non-NULL pointer causes all internal allocations (for the shared backing store, the per-logger write buffer, and the heap message buffer) to use the supplied functions. The allocator is also inherited by all loggers derived from the root via `clog_derive`. The `mprocs` struct is copied internally; the caller may free it after the logger is created.

```c
ccol_memmgmt_procs_t my_alloc = {
    .malloc  = my_malloc,
    .free    = my_free,
    .calloc  = my_calloc,
    .realloc = my_realloc,
};
clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, NULL, NULL, &my_alloc);
```

All four function pointers must be set; passing a partially-populated struct returns `CLOG_INVALID`.

### Async Logging

By default, every `log_*` call formats its message, serializes its fields, and writes to the underlying fd or file synchronously, all before returning. Passing a `clog_async_cfg_t *` to either creation function switches to asynchronous logging: the calling thread only formats its own message and captures its own timestamp/fields, then hands the rest off to a dedicated writer thread that aggregates records from every logger sharing the same target into one buffer, flushed once a size or time threshold is reached. Async mode is inherited by every logger later derived from the root via `clog_derive`.

```c
clog_async_cfg_t acfg = {
    .queue_size          = 0,      /* 0 = unbounded queue; never blocks a caller */
    .flush_buffer_size   = 64 * 1024,
    .flush_interval_ms   = 200,
};
clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, NULL, &acfg, NULL);
```

Unlike `clog_rotation_cfg_t`, a `NULL` `async_cfg` and a non-`NULL` pointer to an all-zero `clog_async_cfg_t` are not equivalent: passing any non-`NULL` pointer enables async mode, with every zero-valued field falling back to its own documented default; only a literal `NULL` keeps logging synchronous.

`queue_size` controls backpressure: `0` uses an unbounded queue that never blocks a caller on `log_*`; a positive value uses a fixed-capacity queue where a full queue blocks the calling thread until space frees up rather than dropping the message. `flush_buffer_size` and `flush_interval_ms` control how often the writer thread actually writes to disk: whichever threshold is reached first triggers a flush. The batching buffer's own growth ceiling follows `flush_buffer_size` itself whenever that is configured above the library's internal 16 MiB per-record default described above, so a larger configured value is genuinely honored, not silently capped at that default. Call `clog_flush` to block until everything queued as of that call has been durably written, without waiting for either threshold:

```c
log_info(lg, "about to do something risky");
clog_flush(lg);   /* block until the line above is on disk */
```

`CLOG_FMT_SYSLOG` output is exempt from batching regardless of these settings: every record and every backtrace frame keeps its own `write()` call, matching the one-write-per-UDP-datagram contract syslog output already documents. A `CLOG_FATAL` call always bypasses the queue: it first drains everything already queued from earlier calls, then writes the fatal record itself synchronously, before terminating the process; a message logged moments before a crash is never silently lost.

Because a batch can span several records built moments apart, a batch that happens to straddle a rotation boundary is written to whichever file is current at flush time, not split precisely at each record's own logging moment.

A logger's async writer thread does not exist in a forked child process; a `fork()`ed child not immediately followed by `exec(3)` transparently falls back to logging synchronously on any inherited async-enabled handle, so `log_*`/`clog_close` continue to work correctly on both sides with no risk of the child waiting on a thread that was never duplicated into it. This fallback is part of the same `FORK_SAFETY_REQUIRED` protection (see "Compile-Time Configuration" in section 4, which also describes the two supported fork(2) patterns) and is not performed when it is compiled out; a caller that disables it must not fork a process with a live async-enabled logger.

### Log Levels

| Level | Value | Notes |
|---|---|---|
| `CLOG_TRACE` | 0 | Finest-grained detail |
| `CLOG_DEBUG` | 1 | |
| `CLOG_INFO`  | 2 | Recommended production minimum |
| `CLOG_WARN`  | 3 | |
| `CLOG_ERROR` | 4 | Appends a backtrace |
| `CLOG_ALERT` | 5 | Action required immediately; maps to RFC 5424 severity 1; appends a backtrace |
| `CLOG_FATAL` | 6 | Appends a backtrace and terminates the process via `exit(EXIT_FAILURE)`; bypasses `min_level`; the message is always written |
| `CLOG_OFF`   | 7 | Disables all output (except a fatal termination) when used as `min_level` |

The minimum level can be changed at any time with `clog_set_level`. Messages below the current minimum are dropped silently, with the sole exception of `CLOG_FATAL` which is always written regardless of `min_level`.

### Structured Fields

Fields are persistent key=value pairs that appear in every subsequent log line. `clog_set_field` updates an existing key in place or prepends a new one; both key and value are copied internally.

```c
clog_set_field(lg, "request_id", "abc-123");
log_info(lg, "processing");
/* -> ts=... request_id=abc-123 msg=processing */

clog_remove_field(lg, "request_id");
log_info(lg, "done");
/* -> ts=... msg=done */

clog_clear_fields(lg);   /* remove all fields */
```

Field values that contain spaces, `=`, `"`, `\`, or control characters are automatically double-quoted and backslash-escaped in the output.

A key must consist entirely of printable US-ASCII characters (0x21-0x7e) excluding `=`, `]`, `"`, and `\`. `clog_set_field` silently ignores a key that is empty, contains any of those excluded characters, contains whitespace or a control character, contains a non-ASCII byte, or that names one of the fixed keys every log line already carries (`ts`, `level`, `proc`, `src`, `func`, `msg`, `bt`, `bt_error`); a field reusing one of those names would otherwise produce a duplicate key in the same record.

### Real-World Use Case: Per-Order Logging for a Small Shop

A small order-processing script logs what happens to every customer order, tagged with that order's ID and customer name. Using `clog_derive`, each order gets a private logger that shares the underlying file and mutex with the root logger but carries its own order-scoped fields. The derived handle is released once the order is done, without affecting the root or any other derived loggers:

```c
clog g_logger;   /* root logger, initialised at startup */

void process_order(const char *order_id, const char *customer, const char *item) {
    clog order_log = clog_derive(g_logger);

    clog_set_field(order_log, "order_id", order_id);
    clog_set_field(order_log, "customer", customer);

    log_info(order_log, "order received: %s", item);

    int rc = ship_item(item);
    if (rc != 0)
        log_error(order_log, "shipping failed with code %d", rc);
    else
        log_info(order_log, "order shipped");

    clog_close(order_log);   /* this handle is released; root logger is unaffected */
}

int main(void) {
    clog_rotation_cfg_t rot = {
        .size_rotation_enabled  = true,
        .max_file_size          = 10L * 1024L * 1024L,   /* 10 MiB */
        .time_rotation_enabled  = true,
        .rotation_interval_secs = 86400,                  /* daily */
        .max_rotated_files      = 14,
    };
    g_logger = clog_open_file_mp("/var/log/orders.log", CLOG_INFO, &rot, NULL, NULL);
    clog_set_field(g_logger, "app", "order-processor");

    process_order("ORD-1001", "Alice", "coffee mug");
    process_order("ORD-1002", "Bob",   "desk lamp");

    clog_close(g_logger);
    return 0;
}
```

Order-scoped fields (`order_id`, `customer`) appear in every line emitted by `order_log` but are absent from lines emitted by the root logger or any other derived logger. All writes share a single mutex, so output from orders processed concurrently is never interleaved.

### Reference: Core Operations

**Lifecycle**

| Function | Description |
|---|---|
| `clog_open_fd_mp(fd, min_level, async_cfg, mprocs)` | Create a logger writing to an existing open fd; the fd is not closed on `clog_close` |
| `clog_open_file_mp(path, min_level, cfg, async_cfg, mprocs)` | Create a file-backed logger; pass a `clog_rotation_cfg_t *` for rotation or `NULL` to disable it |
| `clog_derive(parent)` | Create a derived logger sharing the same fd, mutex, and rotation as `parent`; starts with a snapshot of `parent`'s fields and level, then evolves independently; release with `clog_close` |
| `clog_close(logger)` | Flush, close (if file-backed), and free all resources; the underlying fd is kept open until all derived handles are also closed |
| `clog_flush(logger)` | Block until everything queued as of this call has been durably written; a no-op for a synchronous (non-async) logger |

**Level Control**

| Function | Description |
|---|---|
| `clog_set_level(logger, level)` | Change the minimum log level; thread-safe |
| `clog_get_level(logger)` | Return the current minimum level |

**Output Format and Syslog Facility**

| Function | Description |
|---|---|
| `clog_set_format(logger, fmt)` | Change the output format (`CLOG_FMT_LOGFMT`, `CLOG_FMT_JSON`, or `CLOG_FMT_SYSLOG`); affects all handles sharing the same fd; `CLOG_FMT_SYSLOG` is a no-op on file-backed loggers; thread-safe |
| `clog_get_format(logger)` | Return the current format |
| `clog_set_facility(logger, facility)` | Change the RFC 5424 syslog facility (e.g. `CLOG_SYSLOG_DAEMON`); affects all handles sharing the same fd; only meaningful with `CLOG_FMT_SYSLOG`; default is `CLOG_SYSLOG_USER`; thread-safe |
| `clog_get_facility(logger)` | Return the current syslog facility |

**Structured Fields**

| Function | Description |
|---|---|
| `clog_set_field(logger, key, value)` | Attach a persistent `key=value` field; updates the value if the key already exists; silently ignored for an invalid or reserved key |
| `clog_remove_field(logger, key)` | Remove a field; no-op if the key is absent |
| `clog_clear_fields(logger)` | Remove all attached fields |

**Logging Macros**

| Macro | Level | Backtrace |
|---|---|---|
| `log_trace(lg, fmt, ...)` | `CLOG_TRACE` | No |
| `log_debug(lg, fmt, ...)` | `CLOG_DEBUG` | No |
| `log_info(lg, fmt, ...)`  | `CLOG_INFO`  | No |
| `log_warn(lg, fmt, ...)`  | `CLOG_WARN`  | No |
| `log_error(lg, fmt, ...)` | `CLOG_ERROR` | Yes |
| `log_alert(lg, fmt, ...)` | `CLOG_ALERT` | Yes - use for conditions requiring immediate operator action |
| `log_fatal(lg, fmt, ...)` | `CLOG_FATAL` | Yes - terminates the process via `exit(EXIT_FAILURE)` after writing the log and backtrace; never returns; bypasses `min_level` so the cause is always recorded |

---

## 15. JSON Parser / Serializer / DOM - `cjson`

JSON (JavaScript Object Notation) is a text format for structured data, widely used in web APIs and configuration files. `cjson` parses a JSON string into a tree of nodes held in memory (a DOM; Document Object Model), lets you read and modify any node in the tree, and serialises the result back to a JSON string when you are done.

Two path macros, `cjson_get` and `cjson_set`, let you navigate the tree using a dot-separated path string like `"users.#0.name"` instead of chaining individual lookup calls by hand.

### Node types

Every JSON value is represented by an opaque `cjson` handle.  The type tag is a `cjson_node_type_t` enum:

| Tag | C storage | Meaning |
|---|---|---|
| `CJSON_NULL` | - | JSON `null` |
| `CJSON_BOOL` | `bool` | JSON `true` / `false` |
| `CJSON_INTEGER` | `long long` | JSON number without decimal point or exponent (falls back to `CJSON_FLOAT` on overflow) |
| `CJSON_FLOAT` | `double` | JSON number with decimal point, exponent, or integer value that overflows `long long` |
| `CJSON_STRING` | `char *` (owned copy) | JSON string (UTF-8) |
| `CJSON_LIST` | `cvec` of child `cjson` | JSON array |
| `CJSON_DICTIONARY` | `chmap` of `char * -> cjson` | JSON object |

> **Integer precision:** a JSON number with no decimal point or exponent is stored exactly as a `long long` (`CJSON_INTEGER`) as long as it fits in 64 bits. A larger integer literal (or any number with a decimal point or exponent) is stored as a `double` (`CJSON_FLOAT`) instead, which only represents integers exactly up to 2^53; a round-trip of a bare integer literal larger than `LLONG_MAX` through parse and serialize is not guaranteed to preserve its exact value. This matches the JSON number handling of most mainstream JSON libraries (JSON itself places no bound on numeric precision, but few parsers implement arbitrary-precision numbers by default).

> **Locale independence:** `CJSON_FLOAT` values always parse and serialize with `.` as the decimal separator, per RFC 8259, regardless of the calling thread's ambient `LC_NUMERIC` locale setting (e.g. a locale that uses `,` for its own decimal point). A process that has called `setlocale()`/`uselocale()` elsewhere for its own purposes is unaffected: `cjson_parse`/`cjson_serialize` always produce and consume standard, portable JSON number syntax.

### Construction and parsing

```c
/* cjson_parse_mp accepts memory management functions
   to be used while parsing a given JSON string */
char *err_str = NULL;
cjson doc = cjson_parse_mp("{\"users\":[{\"name\":\"Alice\",\"age\":30}]}", &err_str, NULL);
if (!doc) { fprintf(stderr, "%s\n", err_str); free(err_str); exit(1); }

/* cjson_parse is a convenience wrapper that internally passes
   NULL for memory management procs to cjson_parse_mp. */
cjson doc2 = cjson_parse("{\"users\":[{\"name\":\"Alice\",\"age\":30}]}", &err_str);
if (!doc2) { fprintf(stderr, "%s\n", err_str); free(err_str); exit(1); }

/* One can also pass NULL for err_str when they don't need
   the parsing error details to both of these JSON parsing
   functions. */
cjson doc3 = cjson_parse("{\"foo\": \"bar\"}", NULL);

/* Building programmatically */
cjson arr = cjson_create_list();
cjson_list_push(arr, cjson_create_int(1));
cjson_list_push(arr, cjson_create_string("two"));
```

> **`\u0000` in string values:** The parser rejects the JSON escape sequence `\u0000` and returns `NULL` with a parse error. Because `cjson` stores all string values as null-terminated `char *` buffers, an embedded null byte would silently truncate the string at that position. Rejecting `\u0000` up-front prevents silent data corruption.

> **Nesting depth:** A document nested more than 500 levels deep (arrays and/or objects, in any combination) is rejected with a parse error rather than recursed into further; this bounds worst-case parse-time stack usage against a pathologically or maliciously deeply-nested document. `cjson_clone` and `cjson_serialize`/`cjson_serialize_pretty` enforce the identical 500-level cap independently, since a tree built directly through `cjson_list_push`/`cjson_dictionary_set` is not otherwise bounded in depth the way a parsed document is; both report the cap the same way they report any other allocation failure (`NULL`). Ordinary JSON documents and trees never approach this depth.

### Serialization

```c
/* All of the pointers returned by cjson_serialize* variants
   need to be freed by the callers */
char *compact = cjson_serialize(doc);           /* compact serialization */
char *pretty  = cjson_serialize_pretty(doc, 2); /* 2-space indent */
cjson_serialize_free(compact);
cjson_serialize_free(pretty);
```

`cjson_serialize` and `cjson_serialize_pretty` take no separate allocator parameter: the output buffer is always allocated through the *root node's own* stored allocator (see "Custom memory management" below), so a tree built with `cjson_parse_mp`/`cjson_create_*_mp` is automatically serialized with that same `mp`. Free the returned string with `cjson_serialize_free_mp(s, mp)` (passing that same `mp`), or `cjson_serialize_free(s)` for the default allocator.

A subtree nested more than 500 levels deep (arrays and/or objects, in any combination) is not serialized; both functions return `NULL` instead, the same as any other allocation failure.

### Path navigation - `cjson_get` and `cjson_set`

Paths are dot-separated component strings.  A component that begins with `#` followed by **one or more decimal digits** addresses **an array element by index when the current node is an array**; otherwise it is treated as a **literal object key**.  A bare `#` with no trailing digits, or an empty path component (a leading, trailing, or doubled `.`), is always a syntax error (`cjson_get` returns `NULL`; `cjson_set` and `cjson_delete` return `ccol_invalid_args`).

`cjson_set` creates a leaf that addresses an object key on demand.  A `#N` array-index leaf must already be in range: an array has no way to be auto-extended to fit an arbitrary index, so a syntactically valid but out-of-range one fails with `ccol_key_not_found` instead of being created (matching how a missing intermediate path component is reported).

Two escape sequences are recognised inside path strings:

| Sequence | Meaning in key |
|----------|----------------|
| `\\.`    | A literal `.` character (not a path separator) |
| `\\\\`   | A literal `\` character |

A `\` before any other character is passed through unchanged.

```c
/* Key named "a.b" (literal dot): */
cjson node = cjson_get(doc, "a\\.b");

/* Two-level path where the first key is "a.b" and the second is "c.d": */
cjson node2 = cjson_get(doc, "a\\.b.c\\.d");
```

```c
/* Read values anywhere in the tree */
cjson name = cjson_get(doc, "users.#0.name");
if (cjson_type(name) == CJSON_STRING)
    printf("%s\n", cjson_str_val(name));   /* "Alice" */

/* Write scalars - creates the leaf if absent, changes its type if it exists */
cjson_set(doc, "users.#0.active", (bool)true);
cjson_set(doc, "users.#0.score",  99);
cjson_set(doc, "users.#0.label",  "champion");

/* Replacing an existing subtree (array, object) with a scalar is safe;
   the old subtree is deep-freed automatically. */
cjson_set(doc, "users.#0.name", 42);  /* name is now an integer */
```

`cjson_set` accepts: `bool`, any integer type, `float`, `double`, `char *`, `const char *`, and string literals.  It detects the C type at compile time via `_Generic` and routes to the correct storage path.  Passing `NULL` sets the leaf to `CJSON_NULL`; a typed null pointer such as `(const char *)NULL` also produces `CJSON_NULL` because a null C string pointer maps to JSON null.  A `void *` value that is not NULL (e.g. a `void *` variable holding a live pointer) is rejected with `ccol_invalid_args` rather than being silently written as `CJSON_NULL`: only a genuine NULL is treated as an intentional null.  Non-finite `double` values (`INFINITY`, `-INFINITY`, `NAN`) are rejected and `ccol_invalid_args` is returned; the existing node is left untouched.  Signed integer types (including plain `char` on platforms where `char` is signed, e.g. x86-64 Linux) are sign-extended correctly to `long long`.  Any C type outside this accepted list (`long double`, a struct, an enum, or any pointer type other than `char *` / `const char *`) is rejected the same way: `ccol_invalid_args` is returned and the target leaf is left untouched, rather than silently written as `CJSON_NULL`.

Duplicate object keys set within the JSON object use **last-value-wins** semantics; the final occurrence of a key is retained and prior occurrences are deep-freed.

### Real-World Use Case: Marking a To-Do Item as Done

A small to-do list application stores each task as a JSON file. When the user checks a task off, the program needs to validate that the file has the field it expects, flip the task's status, and stamp it with the time it was completed. The `cjson_get` and `cjson_set` path macros allow targeted updates deep in the tree without rebuilding the whole document:

```c
char *mark_task_done(const char *raw_json) {
    char *err = NULL;
    cjson doc = cjson_parse(raw_json, &err);
    if (!doc) {
        fprintf(stderr, "parse error: %s\n", err);
        free(err);
        return NULL;
    }

    /* Validate a required field */
    cjson title = cjson_get(doc, "task.title");
    if (!title || cjson_type(title) != CJSON_STRING) {
        cjson_destroy(doc);
        return NULL;
    }

    /* Overwrite the status field in place - old node is deep-freed automatically */
    cjson_set(doc, "task.status", "done");

    /* Stamp when the task was completed */
    cjson_set(doc, "task.completed_at", (long long)time(NULL));

    char *out = cjson_serialize(doc);
    cjson_destroy(doc);
    return out;   /* caller must call cjson_serialize_free(out) */
}
```

`cjson_get` returns a non-owning reference into the live tree, valid until the tree is mutated or destroyed. `cjson_set` deep-frees the old node at the target path before installing the new value, so replacing a nested object or array with a scalar is always safe and leak-free.

### Deleting nodes - `cjson_delete`

`cjson_delete(root, path)` removes the node addressed by the path and recursively frees its entire subtree.  For dictionary parents the leaf is addressed by key; for array parents the leaf must be a `#N` component.

```c
cjson_delete(doc, "users.#0.address");   /* remove a nested object */
cjson_delete(doc, "config.debug");       /* remove a scalar key */
cjson_delete(doc, "items.#2");           /* remove an array element */
```

The two lower-level functions are also available when you already hold a reference to the immediate parent:

```c
/* Remove by index from an array you already have a handle to */
cjson arr = cjson_get(doc, "items");
ccol_retval_t r = cjson_list_remove(arr, 0);

/* Remove by key from an object you already have a handle to */
cjson obj = cjson_get(doc, "config");
ccol_retval_t r2 = cjson_dictionary_remove(obj, "debug");
```

`cjson_list_remove` shifts all subsequent elements left and shrinks the backing array.  `cjson_dictionary_remove` returns `ccol_key_not_found` when the key does not exist.

### Value access

```c
cjson_node_type_t cjson_type(cjson node);
bool        cjson_bool_val(cjson node);
long long   cjson_int_val(cjson node);
double      cjson_double_val(cjson node);
const char *cjson_str_val(cjson node);   /* string owned by the node */
size_t      cjson_list_len(cjson node);
size_t      cjson_dictionary_size(cjson node);
```

### Destruction and ownership

```c
cjson_destroy(doc);   /* deep-frees the entire tree, NULLs the handle */
```

- `cjson_list_push` and `cjson_dictionary_set` transfer ownership of the child on every outcome that touches it at all: on success the array/object owns it; on a failure caused by an invalid target (`arr`/`obj` NULL or the wrong type, or a NULL `key`) or an internal insertion failure, a freshly unattached child is deep-freed and must not be freed by the caller. The one class of failure that leaves the child completely untouched, still owned by whatever it was already attached to, is described next; this holds regardless of what else is wrong with the call, so an invalid `arr`/`obj`/`key` never causes an already-attached child to be destroyed.
- The `child` passed to `cjson_list_push`/`cjson_dictionary_set` must not already be attached to a list/dictionary parent; it must be a freshly created node or a fresh `cjson_clone()`, never a **borrowed** reference from `cjson_get()`/`cjson_list_get()`/`cjson_dictionary_get()`, never the target container itself, and never a node removed via `cjson_list_remove()`/`cjson_dictionary_remove()` (both always deep-free the node they remove and never hand back a live reference to it). Passing an already-attached node is rejected with `ccol_invalid_args` and leaves it completely untouched (still owned by whatever it was already attached to); accepting it would give the same node two owners, each independently freeing it when its own parent is destroyed. The one exception is `cjson_dictionary_set(obj, k, cjson_dictionary_get(obj, k))` (setting a key to its own current value), which is a harmless no-op.
- `child` must also not already contain the target container somewhere within its own subtree; attaching it would make the target both a new ancestor of `child` and an existing descendant of it, a cycle. This is rejected the same way (`ccol_invalid_args`, `child` untouched); if the check itself cannot complete under memory pressure, `ccol_not_enough_memory` is returned instead of silently risking an undetected cycle.
- `cjson_get` returns a **non-owning** reference valid until the tree is mutated or destroyed.
- `cjson_clone` returns a fully independent deep copy, or `NULL` on allocation failure (including a source nested more than 500 levels deep; see the "Nesting depth" note above). Cloning a borrowed reference first is how to safely move a value that already lives somewhere else in a tree into a new location.

### Custom memory management

Every factory and parse function has an `_mp` variant that accepts a `ccol_memmgmt_procs_t *mp` parameter.  The names without `_mp` suffix are `static inline` wrappers that pass `NULL` (default `malloc`/`free`/`calloc`/`realloc`).

```c
/* All nodes in the tree use my_procs. */
char *err = NULL;
cjson doc = cjson_parse_mp(json_str, &err, &my_procs);
if (!doc) { fprintf(stderr, "%s\n", err); cjson_serialize_free_mp(err, &my_procs); /* handle error */ }
/* ... use doc ... */
cjson_destroy(doc);   /* uses each node's stored allocator automatically */

/* Serialization buffer is allocated with the root node's allocator. */
char *out = cjson_serialize(doc2);
cjson_serialize_free_mp(out, &my_procs);   /* pass same mp used at creation */

/* Build a tree programmatically with a custom allocator. */
cjson root = cjson_create_dictionary_mp(&my_procs);
cjson_dictionary_set(root, "x", cjson_create_int_mp(42, &my_procs));
cjson_destroy(root);
```

**Per-node ownership:** the allocator is stamped on every node at creation time.  `cjson_destroy()` uses each node's own stored allocator; no external `mp` parameter is needed for destruction.  `cjson_clone()` inherits the allocator from the source tree.

**Node pool:** `cjson` maintains a per-thread free-list (capped at 512 nodes) to amortize allocation cost for the common case.  Custom-allocator nodes (`mp != NULL`) bypass the pool entirely and are allocated/freed directly through their own allocator.  Default-allocator nodes (`mp == NULL`) use the pool as usual; it is drained at thread exit with plain `free()`.

---

## 16. YAML Parser / Serializer / DOM - `cyaml`

`cyaml` parses a YAML document into a mutable tree of nodes, lets you read and modify those nodes, and serialises the result back to YAML. The API mirrors `cjson` closely: the same dot-separated path macros (`cyaml_get` and `cyaml_set`) work on YAML trees using the same syntax.

### Supported YAML features

| Feature | Notes |
|---|---|
| Block mappings | Indentation-sensitive key: value pairs, implicit (`key:`) and explicit (`? key` / `: value`) styles, freely mixed within one mapping |
| Block sequences | Indentation-sensitive `- item` lists; a sequence value may sit at the same indentation as its parent mapping key |
| Flow mappings | `{key: value, ...}` inline style, including a bare key with no `:` (value is `null`), a `:` with no value (also `null`), and an explicit `{? key: value}` entry whose key may itself span multiple lines |
| Flow sequences | `[a, b, c]` inline style, including a bare `key: value` pair as a shorthand for a single-entry mapping element (`[foo: bar]` == `[{foo: bar}]`) and an explicit `[? key: value]` entry whose key may itself span multiple lines |
| Plain scalars | Unquoted values; may span multiple lines, folded to a single space per line break (blank lines fold to newlines instead), following the same rules as a folded block scalar's own line folding |
| Single-quoted scalars | No escape processing; `''` encodes a literal `'`; may span multiple lines with the same folding rules as a plain scalar |
| Double-quoted scalars | Full YAML escape sequences including `\uXXXX`; may span multiple lines with the same folding rules as a plain scalar |
| Literal block scalars | `|`; newlines preserved |
| Folded block scalars | `>`; newlines folded to spaces except blank lines |
| Block scalar chomping | `|+` keep, `|-` strip, `|` clip (default) |
| Anchors and aliases | `&name` / `*name` on any node, including a mapping key itself; aliases resolve to deep clones; scoped per document; a node may carry at most one anchor and one tag (in either order), and an alias may never itself carry either |
| Non-scalar dictionary keys | A sequence, mapping, or flow collection used directly as a dictionary key is canonicalized to its flow-style text representation (a mapping's own entries in sorted, lexicographic-by-key order at every nesting level, so construction order never affects the result), since this DOM's dictionaries always map `char * -> cyaml` |
| Scalar dictionary key canonicalization | A key's own core-schema type is resolved before it is stored, exactly like an ordinary value position: `~`, `null`, `Null`, `NULL`, and an empty key all canonicalize to `"null"`; `true`/`True`/`TRUE`/`yes`/`on` (and their `false`/`no`/`off` counterparts) canonicalize to `"true"`/`"false"`; `0x10` canonicalizes to `"16"`. This is applied identically to an implicit key (`key:`), an explicit key (`? key`), a flow dictionary key, and a flow sequence's `key: value` shorthand, so every spelling of the same value collides on the same dictionary key regardless of which of the four key notations produced it. A quoted key (`"key"`, `'key'`) is never subject to this: its value is always exactly its own decoded text. |
| Dictionary key order (serialization) | `cyaml_serialize()`/`cyaml_serialize_flow()` emit a dictionary's entries in this DOM's own internal storage order, which is unrelated to insertion order, parse order, or any other caller-visible ordering; a `CYAML_LIST`'s element order, by contrast, is always exactly the order its elements were pushed or parsed in. A caller needing a specific, stable key order in serialized output must arrange for it itself (e.g. by re-inserting keys into a fresh dictionary in the desired order immediately before serializing) |
| Nesting limits | A document nesting mappings/sequences/explicit keys more than 500 levels deep, or a non-scalar dictionary key whose canonical text exceeds 64 KiB, is rejected with a parse error; `cyaml_serialize()`/`cyaml_serialize_flow()`/`cyaml_clone()` each carry the identical 500-level limit independently, since a tree need not have come from parsing at all, and return `NULL` for a tree nested past it |
| Node count limit | A single `cyaml_parse()`/`cyaml_parse_n()` call rejects a document that would require allocating more than 4,000,000 DOM nodes in total, with a parse error naming the limit explicitly; reachable only by a document with millions of distinct scalar values, an extent no realistic hand-written or generated document approaches. Does not apply to a tree built directly via `cyaml_create_list()`/`cyaml_dictionary_set()` outside of parsing |
| YAML 1.2 core schema | Implicit type resolution for null/bool/int/float |
| `%YAML` directive | Validated against the `MAJOR.MINOR` grammar; at most one per document |
| Leading `---` / trailing `...` | Document-start and document-end markers; block-structured content (a mapping or sequence) must start on its own line after `---`, not share its line, though a scalar or flow collection may |
| Multi-document streams | Supported; see below |
| Comments | `#` to end-of-line; silently ignored |
| Tags | `!!str`, a custom `!foo`, or a `%TAG`-resolved shorthand/verbatim `!<...>` tag; resolved and queryable via `cyaml_node_tag()`; see "Tags" below |
| `%TAG` directive | Shorthand-prefix scoping, tracked per document |
| Merge keys | `<<:`; see "Merge keys" below |
| Tab-as-indentation validation | A tab is rejected wherever it would be interpreted as block-structural indentation or a structural separator (leading indentation for a mapping/sequence/block-scalar line, immediately after a `-` / `?` / `:` indicator, or on a flow collection's own continuation line); a tab is accepted as ordinary whitespace everywhere else, including same-line separators, comments, quoted scalars, and literal content once a block scalar's own indentation is already established |

**Not supported:** embedded null bytes in strings (base64-encode any data that may contain one instead; `!!binary` is not given special decoding behavior for the same reason). Since a node's scalar value is a plain null-terminated `char *` with no separate length field, an embedded null byte would silently truncate the value; a double-quoted escape that would decode to codepoint zero (`\0`, `\x00`, `\u0000`, `\U00000000`) is therefore rejected as a parse error rather than silently truncating. Any other raw, unescaped C0 control byte (`0x00`-`0x1F` other than tab) or DEL (`0x7F`) is likewise rejected wherever it appears literally in scalar content, in any scalar style; a double-quoted scalar's own escape sequences (e.g. `\x01`, `\x7f`) remain the way to embed one of these bytes. Also not supported: a tag decorating a dictionary key (parsed for validity, but discarded, since this DOM's keys are plain strings, not nodes); a custom tag's original `%TAG` shorthand spelling across a serialize round-trip (always re-emitted in its fully-resolved, verbatim `!<...>` form); character encoding validation (input is treated as an opaque byte string, so a byte sequence that is not valid UTF-8 is accepted as literal scalar content and reproduced unchanged on serialize, since only a raw C0 control byte other than tab, or DEL, is ever rejected outright).

An implicit block mapping key (`key: value`, with no `?`) must always fit on a single physical line, whether the key is a plain scalar, a quoted scalar, or a non-scalar flow collection; unlike an implicit value, which may fold across multiple lines. An explicit key (`? key`) has no such restriction.

### Multi-document streams

When the input contains multiple `---`-delimited documents, `cyaml_parse` (and its variants) returns a single `CYAML_LIST` node whose elements are the individual document roots in order.  A single-document input is always returned as its root node directly; no wrapping list.

```c
/* Kubernetes-style multi-document YAML */
const char *stream =
    "---\n"
    "kind: Service\n"
    "name: frontend\n"
    "---\n"
    "kind: Deployment\n"
    "name: backend\n";

char *err = NULL;
cyaml root = cyaml_parse(stream, &err);
/* root is CYAML_LIST with 2 elements */
cyaml doc0 = cyaml_list_get(root, 0);   /* {kind: Service,     name: frontend} */
cyaml doc1 = cyaml_list_get(root, 1);   /* {kind: Deployment,  name: backend}  */
cyaml_destroy(root);
```

Rules for multi-document streams:
- The first document may appear with or without a leading `---` marker.
- A later document may also omit `---` **only** when the document immediately
  before it ended with an explicit `...` marker; otherwise it **must** begin
  with `---`. Bare content directly following a document with no `...` is
  ambiguous and is a parse error.
- An optional `...` end marker may follow each document.
- Anchors are scoped per document; a `*alias` cannot reference an `&anchor`
  from a different document.
- An empty document (e.g. two consecutive `---` markers) yields a `CYAML_NULL`
  element in the list.

### Tags

```c
char *err = NULL;
cyaml doc = cyaml_parse("val: !!str 42\nother: !mytag foo\n", &err);

cyaml val = cyaml_dictionary_get(doc, "val");
cyaml_type(val);            /* CYAML_STRING, not CYAML_INTEGER: !!str forced it */
cyaml_str_val(val);         /* "42" */
cyaml_node_tag(val);        /* "tag:yaml.org,2002:str", == CYAML_TAG_STR */

cyaml other = cyaml_dictionary_get(doc, "other");
cyaml_type(other);          /* CYAML_STRING (implicit typing; !mytag doesn't force one) */
cyaml_node_tag(other);      /* "!mytag" */

cyaml_destroy(doc);
```

One of the five scalar core-schema tags (`!!null`, `!!bool`, `!!int`, `!!float`, `!!str`) forces that type on any scalar style, overriding whatever type the text would otherwise implicitly resolve to. `!!bool`, `!!int`, and `!!float` each validate their own scalar's text, reporting a hard parse failure for the whole document on a mismatch (e.g. `!!int` on text that isn't a valid integer); a scalar core-schema tag on actual collection syntax is likewise always a hard failure. `!!null` is the one exception: since there is no canonical "wrong" spelling for a value whose whole point is to carry no further information, `!!null` forces `CYAML_NULL` unconditionally, on any text whatsoever, exactly like a custom tag decorating a scalar never validates what it is attached to. `!!bool` accepts a wider, case-insensitive vocabulary than implicit bool typing (`true`/`false`, `yes`/`no`, `on`/`off`, but not single-letter `y`/`n`). A custom tag (anything other than the seven core-schema URIs, including `!!binary`) never forces a type; it is attached as metadata to whatever type the value already resolved to. `cyaml_node_tag()` returns `NULL` for an untagged node; `cyaml_node_set_tag()` attaches or clears a tag on a programmatically-constructed node without coercing its type. `CYAML_TAG_NULL`/`_BOOL`/`_INT`/`_FLOAT`/`_STR`/`_SEQ`/`_MAP` name the seven core-schema URIs. A core-schema tag is re-emitted by `cyaml_serialize()`/`cyaml_serialize_flow()` only when it does not match the node's actual type (this serializer's own output already round-trips a matching one unaided, so re-emitting it would be pure redundancy); a mismatched core-schema tag (only reachable by attaching one via `cyaml_node_set_tag()` to a node of a different type than the tag names) is preserved exactly like a custom tag, in its fully-resolved verbatim form, so it is never silently lost.

### Merge keys

```c
char *err = NULL;
cyaml doc = cyaml_parse(
    "defaults: &defaults\n"
    "  timeout: 30\n"
    "  retries: 3\n"
    "service:\n"
    "  <<: *defaults\n"
    "  timeout: 60\n",  /* explicit key wins over the merged-in default */
    &err);

cyaml service = cyaml_dictionary_get(doc, "service");
cyaml_int_val(cyaml_dictionary_get(service, "timeout"));  /* 60 */
cyaml_int_val(cyaml_dictionary_get(service, "retries"));  /* 3, merged in */
cyaml_dictionary_get(service, "<<");                      /* NULL: expanded and removed */

cyaml_destroy(doc);
```

A mapping entry whose key is the literal, unquoted, untagged plain scalar `<<` expands its value (a single mapping, or a sequence of mappings, e.g. `<<: [*a, *b]`) into that mapping in place, then removes the `<<` key itself. This applies equally in any mapping regardless of how it arose: the ordinary implicit style (`<<: *defaults`), the explicit `? <<` / `: value` style (block or flow), or the single-entry mapping produced by a block or flow sequence's own `key: value` compact-mapping shorthand (`- <<: *defaults`, `[<<: *defaults]`, `[? <<: *defaults]`); an anchor decorating the `<<` key itself does not disqualify it. Explicit keys already present always win over anything merged in; for a sequence of sources, earlier sources win over later ones on conflict. A merge source that is neither a mapping nor a sequence of mappings is a hard parse failure. A quoted (`"<<"`) or explicitly tagged (`!!str <<`) key spelled `<<` is always an ordinary literal key, never expanded, in any of those forms.

### Node types

Every YAML value is represented by an opaque `cyaml` handle.  The type tag is a `cyaml_node_type_t` enum:

| Constant | Internal storage | YAML kind |
|---|---|---|
| `CYAML_NULL` | none | `null`, `~`, or empty value |
| `CYAML_BOOL` | `bool` | `true` / `false` and case variants |
| `CYAML_INTEGER` | `long long` | Decimal, `0x` hex, `0o` octal; a literal outside the signed 64-bit range falls back to `CYAML_FLOAT` (or `CYAML_STRING`) rather than wrapping around |
| `CYAML_FLOAT` | `double` | Decimal, exponent, `.inf`, `-.inf`, `.nan` |
| `CYAML_STRING` | `char *` (heap) | All non-null scalars that do not match other rules |
| `CYAML_LIST` | `cvec` of child `cyaml` | Ordered list |
| `CYAML_DICTIONARY` | `chmap` of `char * -> cyaml` | Key/value map |

### Parsing

```c
char *err = NULL;

/* cyaml_parse_mp accepts memory management functions for all allocations. */
cyaml doc = cyaml_parse_mp("name: Alice\nage: 30\n", &err, NULL);

/* cyaml_parse is a convenience wrapper that passes NULL for memory procs. */
cyaml doc2 = cyaml_parse("name: Alice\nage: 30\n", &err);

if (!doc2) {
    fprintf(stderr, "parse error: %s\n", err ? err : "unknown");
    free(err);
}

/* cyaml_parse_n / cyaml_parse_n_mp accept a byte length for non-null-terminated input. */
cyaml doc3 = cyaml_parse_n(buf, len, NULL);

/* Multi-document input: the returned node is a CYAML_LIST of document roots. */
cyaml stream = cyaml_parse("---\nfoo: 1\n---\nbar: 2\n", &err);
/* cyaml_type(stream) == CYAML_LIST, cyaml_list_len(stream) == 2 */
```

### Programmatic construction

```c
cyaml root = cyaml_create_dictionary();
cyaml_dictionary_set(root, "name",   cyaml_create_string("Alice"));
cyaml_dictionary_set(root, "age",    cyaml_create_int(30));
cyaml_dictionary_set(root, "active", cyaml_create_bool(true));

cyaml seq = cyaml_create_list();
cyaml_list_push(seq, cyaml_create_int(1));
cyaml_list_push(seq, cyaml_create_string("two"));
cyaml_dictionary_set(root, "items", seq);
```

### Implicit type resolution (YAML 1.2 core schema)

The parser resolves unquoted scalars according to the YAML 1.2 core schema:

| Input | Resolved type |
|---|---|
| `~`, `null`, `Null`, `NULL`, or empty | `CYAML_NULL` |
| `true`, `True`, `TRUE` | `CYAML_BOOL` (true) |
| `false`, `False`, `FALSE` | `CYAML_BOOL` (false) |
| Decimal integer, `0xHEX`, `0oOCTAL` | `CYAML_INTEGER` |
| Decimal float, scientific notation | `CYAML_FLOAT` |
| `.inf`, `-.inf`, `.nan` (any case) | `CYAML_FLOAT` |
| Anything else | `CYAML_STRING` |

To force a value to be treated as a string regardless of its content, use single or double quotes in the YAML source.

### Serialization

```c
/* Block style; human-readable YAML */
char *block = cyaml_serialize(doc);

/* Flow style; compact single-line YAML */
char *flow = cyaml_serialize_flow(doc);

/* Both return heap-allocated strings that must be freed. */
cyaml_serialize_free(block);
cyaml_serialize_free(flow);

/* cyaml_serialize() uses the node's own stored allocator automatically.
 * When the tree was created with a custom allocator, free with _mp: */
char *block2 = cyaml_serialize(doc);
cyaml_serialize_free_mp(block2, mp);
```

### Path navigation (`cyaml_get` and `cyaml_set`)

Paths are dot-separated component strings.  A component that begins with `#` followed by one or more decimal digits addresses a sequence element by index when the current node is a sequence; otherwise it is treated as a literal mapping key.

Two escape sequences are recognised inside path strings:

| Sequence | Meaning in key |
|----------|----------------|
| `\\.`    | A literal `.` character (not a path separator) |
| `\\\\`   | A literal `\` character |

A `\` before any other character is passed through unchanged.

An empty path component (nothing between two dots, or a leading/trailing
dot, e.g. `"a..b"` or `"a."`) has no valid interpretation as either a
literal empty-string key or a `#N` index: `cyaml_get` returns NULL for it,
and `cyaml_set`/`cyaml_delete` return `ccol_invalid_args`, at any position
in the path, not just the leaf.

A NULL or empty path passed to `cyaml_get` returns `root` itself, so a
zero-component path addresses the root the same way a leaf-only path (e.g.
`"key"`) addresses a direct child of it.

```c
/* Key named "a.b" (literal dot): */
cyaml node = cyaml_get(doc, "a\\.b");

/* Two-level path where the first key is "a.b" and the second is "c.d": */
cyaml node2 = cyaml_get(doc, "a\\.b.c\\.d");
```

```c
cyaml name = cyaml_get(doc, "users.#0.name");
if (cyaml_type(name) == CYAML_STRING)
    printf("%s\n", cyaml_str_val(name));   /* "Alice" */

/* cyaml_set creates the key if it does not exist. */
cyaml_set(doc, "users.#0.active", (bool)true);
cyaml_set(doc, "users.#0.score",  99);
```

`cyaml_set` creates a leaf that addresses a dictionary key on demand.  A `#N` list-index leaf must already be in range: a list has no way to be auto-extended to fit an arbitrary index, so an out-of-range one fails with `ccol_key_not_found` instead of being created.  Creating a new leaf dictionary key can also fail with `ccol_container_full` if the parent dictionary has reached its maximum representable element count (not reachable in practice).

`cyaml_set` accepts: `bool`, any integer type, `float`, `double`, `char *`, `const char *`, and string literals.  It detects the C type at compile time via `_Generic` and routes to the correct storage path.  Passing an untyped `NULL` literal sets the leaf to `CYAML_NULL`.  Any C type outside this accepted list (`long double`, a struct, an enum, or any pointer type other than `char *` / `const char *`) is rejected: `ccol_invalid_args` is returned and the target leaf is left untouched, rather than silently written as `CYAML_NULL`.

### Deleting nodes (`cyaml_delete`)

`cyaml_delete(root, path)` removes the node addressed by the path and recursively frees its entire subtree.  For dictionary parents the leaf is addressed by key; for list parents the leaf must be a `#N` component.

```c
cyaml_delete(doc, "server.debug");       /* remove a scalar key */
cyaml_delete(doc, "hosts.#0");           /* remove a list element */
cyaml_delete(doc, "users.alice.address"); /* remove a nested mapping */
```

The two lower-level functions are also available when you already hold a reference to the immediate parent:

```c
/* Remove by index from a list you already have a handle to */
cyaml seq = cyaml_get(doc, "hosts");
ccol_retval_t r = cyaml_list_remove(seq, 0);

/* Remove by key from a dictionary you already have a handle to */
cyaml server = cyaml_get(doc, "server");
ccol_retval_t r2 = cyaml_dictionary_remove(server, "debug");
```

`cyaml_list_remove` shifts all subsequent elements left and shrinks the backing array.  `cyaml_dictionary_remove` returns `ccol_key_not_found` when the key does not exist.

### Duplicate mapping keys

The parser accepts duplicate keys without error.  When the same key appears more than once in a mapping, the last value wins and the earlier value is silently freed.  This matches common YAML parser behavior but is not mandated by the YAML 1.2 specification.  Callers should not rely on duplicate-key detection; treat it as implementation-defined behavior.

### Anchors and aliases

```c
const char *yaml =
    "defaults: &base\n"
    "  timeout: 30\n"
    "  retries: 3\n"
    "staging: *base\n";

cyaml doc = cyaml_parse(yaml, NULL);
cyaml timeout = cyaml_get(doc, "defaults.timeout");
/* CYAML_INTEGER, value 30 */
cyaml st_timeout = cyaml_get(doc, "staging.timeout");
/* Independent deep clone: also CYAML_INTEGER, value 30 */
```

Aliases resolve to independent deep clones of the anchored node.  Modifying the alias does not affect the original.

### Lifecycle

```c
/* Manual destroy: */
cyaml_destroy(doc);    /* frees all nodes; NULLs the pointer */

/* RAII (GCC/Clang only): */
cyaml_declare_scoped(doc2);
doc2 = cyaml_parse("x: 1\n", NULL);
/* doc2 is automatically freed when it goes out of scope */
```

### Custom allocators

```c
cyaml_declare(doc);
doc = cyaml_parse_mp(yaml, &err, &my_mprocs);
cyaml_dictionary_set(doc, "key", cyaml_create_string_mp("val", &my_mprocs));
cyaml_destroy(doc);
```

The allocator is stamped on every node at creation time.  `cyaml_destroy()` uses each node's own stored allocator.  `cyaml_clone()` inherits the allocator from the source tree, and each node's own tag, if any, is carried over unchanged.

**Node pool:** `cyaml` maintains a per-thread free-list (capped at 512 nodes) to amortize allocation cost.  Custom-allocator nodes bypass the pool entirely.  Default-allocator nodes use the pool; it is drained at thread exit with plain `free()`.

---

## 17. Thread Pool - `cthreadpool`

Creating a new OS thread for every task is expensive: each `pthread_create` call allocates a stack and involves a system call. A thread pool solves this (for ephemeral threads to perform some work in parallel and terminate) by creating a fixed number of worker threads once and reusing them. You submit tasks to a queue; the next available worker picks one up. This bounds concurrent thread count and eliminates the per-task creation overhead.

`cthreadpool` is a generic thread pool with a bounded or unbounded task queue, optional completion callbacks, and futures. A future is an object that lets the submitting thread collect a `void *` result from a task after it has finished running on a worker thread. Worker threads are created at construction and remain alive until the pool is shut down.

The queue mode is selected once at construction time by the `queue_capacity` parameter:

- `queue_capacity == 0` or `ccol_invalid_size` - unbounded queue: `ctpool_submit` never blocks on capacity; the only non-trivial failure path is out-of-memory.
- `queue_capacity > 0` - bounded queue of that capacity: `ctpool_submit` blocks when the queue is full, applying natural backpressure to producers.

`ctpool` is an opaque VALUE handle, not a pointer: it must never be cast to/from `void *`, compared via a pointer cast, or treated as an address. Compare it against `CTPOOL_INVALID` (or use a truthiness check; `CTPOOL_INVALID` is `0`, so `if (!pool)` works as expected). Internally, every use of a `ctpool` is resolved through a library-owned slot table before the underlying pool object is touched, so a stale handle (one whose pool has already been destroyed) is always detected rather than silently dereferencing freed memory; passing an already-destroyed or otherwise stale handle to `ctpool_destroy`/`__ctpool_destroy` specifically is a fatal error (`abort()`/`SIGABRT`), covering both a purely sequential double-destroy and a concurrent one, rather than risking a double-free.

Calling `ctpool_destroy`/`__ctpool_destroy` on `pool` from within a task (or that task's `on_complete` callback) currently executing on one of `pool`'s own worker threads is likewise a fatal error, for the same reason a stale handle is: a worker thread cannot join itself, so destroying its own pool from inside it would free the pool's memory while that worker is still using it. `ctpool_wait`, `ctpool_shutdown_drain`, and `ctpool_shutdown_immediate` handle this same situation gracefully instead of aborting: called this way, `ctpool_wait` returns immediately (waiting for the calling task's own completion would deadlock it against itself) and the two shutdown functions are a complete no-op (neither stops accepting new tasks nor joins any worker), leaving the pool fully usable so that a later, legitimate external call can still shut it down cleanly.

The supported way to combine `fork(2)` with `cthreadpool` is one of the two patterns described under "Compile-Time Configuration" in section 4: `fork(2)` before creating a pool, or create one and `fork(2)` immediately followed by `exec(3)`. As a defense-in-depth measure on top of that, a fresh `create_cthread_pool`/`_mp`/`ctpool_destroy`/`ctpool_submit`/`_try_submit`/`_timed_submit` (and the future/wait/shutdown variants) call, from any thread, in either the parent or a freshly forked child, never hangs waiting on a lock that some other (possibly no-longer-existing, since `fork()` duplicates only the calling thread) thread happened to hold at the instant of the fork. A pool that was already live across the fork is a different matter regardless of that measure: `fork()` does not duplicate its worker threads, so an inherited pool can no longer run any queued or future task in the child; treat such a pool as inert there (not usable for further submission) rather than keep submitting to it. `ctpool_wait` on such a pool in the child returns immediately rather than waiting for progress that can never happen, and `ctpool_destroy` still discards any work still queued at the moment of the fork exactly as `ctpool_shutdown_immediate` would (cancelling any queued futures, so a caller blocked in `ctpool_future_get` on one of them wakes up rather than hanging) before releasing the pool's own memory, even when called with no prior explicit shutdown call. This protection can be compiled out via `FORK_SAFETY_REQUIRED=0` for a caller that has no need for it.

With a custom allocator (`create_cthread_pool_mp`), the small internal bookkeeping node backing each submitted task is recycled through a bounded per-pool cache rather than allocated and freed on every single submission; a caller relying on the supplied allocator to observe exactly one allocation and one free per submitted task (for example a tracking or instrumenting allocator) will instead see calls only when the cache is empty or already full.

**Header:** `#include <cthreadpool.h>`

### Basic Usage

```c
/* Four worker threads, bounded queue of 256 tasks */
ctpool_construct(pool, 4, 256);

void compute(void *arg) { /* ... */ }
void on_done(void *arg)  { /* called by the worker after compute returns */ }

ctpool_submit(pool, compute, my_arg, on_done);

/* Wait for all queued and active tasks to finish
   without shutting down the pool */
ctpool_wait(pool);

/* Graceful shutdown: drain remaining tasks, then stop workers */
ctpool_shutdown_drain(pool);
ctpool_destroy(pool);
```

### Task Submission

Three submission variants cover different producer patterns:

```c
/* Blocking: waits for space if the bounded queue is full */
ccol_retval_t rc = ctpool_submit(pool, fn, arg, on_complete);

/* Non-blocking: returns ccol_container_full immediately if full */
rc = ctpool_try_submit(pool, fn, arg, on_complete);

/* Timed: blocks up to the given relative duration */
struct timespec timeout = { .tv_sec = 1, .tv_nsec = 0 };
rc = ctpool_timed_submit(pool, fn, arg, on_complete, &timeout);
```

The `on_complete` callback is invoked by the worker immediately after `fn` returns and receives the same `arg`. It may be `NULL`.

### Futures

A future lets the submitting thread collect a `void *` result from a task:

```c
void *heavy_compute(void *arg) {
    /* ... process ... */
    return result;
}

ctpool_future *f = ctpool_submit_future(pool, heavy_compute, arg);
if (!f) { /* OOM or pool is shutting down */ }

void *result = ctpool_future_get(f);   /* blocks until the worker finishes */
ctpool_future_free(f);                 /* release the caller's reference */
```

> **Result ownership:** the pool never allocates, copies, or frees the `void *` result; it only carries the pointer `fn` returned. `ctpool_future_free` releases the future's own handle, never the result. Once `ctpool_future_get` returns, the caller owns the result and is responsible for freeing it however `fn` allocated it.

`ctpool_submit_future` blocks when a bounded queue is full. The same non-blocking and timed variants available for regular tasks also exist for futures:

```c
/* Non-blocking: returns ccol_container_full immediately if queue is full */
ctpool_future *f = NULL;
ccol_retval_t rc = ctpool_try_submit_future(pool, heavy_compute, arg, &f);

/* Timed: blocks at most timeout waiting for a free slot */
struct timespec timeout = { .tv_sec = 1, .tv_nsec = 0 };
rc = ctpool_timed_submit_future(pool, heavy_compute, arg, &timeout, &f);

/* NULL timeout is equivalent to ctpool_try_submit_future */
rc = ctpool_timed_submit_future(pool, heavy_compute, arg, NULL, &f);
```

Both functions set `*out` to the future handle on success and to `NULL` on any failure, returning a `ccol_retval_t` that distinguishes between OOM, shutdown, container-full, and timeout.

The future carries a reference count of 2 at submission: one for the caller and one held by the queued task. Calling `ctpool_future_free` before `ctpool_future_get` gives fire-and-forget semantics: the future is freed automatically once the worker finishes, and the result is discarded.

Multiple threads may each hold a pointer to the same future and call `ctpool_future_get` independently. Each must call `ctpool_future_free` exactly once.

### Detached Futures

A detached future is the same `ctpool_future` handle, but created and fulfilled without any `cthread_pool` involved at all; useful when some other kind of external, event-driven producer (not a ctpool worker thread) needs to hand a `void *` result back to a waiting caller using the same wait/poll/free API as a pool-backed future:

```c
char *err = NULL;
ctpool_future *f = ctpool_future_create_detached(&err);
if (!f) { /* OOM */ }

/* Handed off to, say, a reactor thread or any other producer: */
ctpool_future_fulfill(f, result);   /* called exactly once by the producer */

/* Meanwhile, the caller: */
void *result = ctpool_future_get(f);   /* blocks until fulfilled */
ctpool_future_free(f);                 /* release the caller's reference */
```

Like a pool-backed future, a detached future starts with a reference count of 2 (caller + producer) and is freed once both sides have released their reference, in either order; `ctpool_future_free` may be called before `ctpool_future_fulfill`, giving the same fire-and-forget semantics as the pool-backed case. `ctpool_future_fulfill` returns `ccol_not_permitted` if called a second time on the same future; it is a one-shot handoff, not a mutable slot. Do not call `ctpool_future_fulfill` on a future returned by `ctpool_submit_future`/`ctpool_try_submit_future`/`ctpool_timed_submit_future`; those are fulfilled internally by the worker thread that runs their task.

### Phase Synchronization

`ctpool_wait` blocks until the queue is empty and all active tasks have completed, without shutting the pool down. New tasks may be submitted after it returns.

```c
/* Submit phase 1 */
for (size_t i = 0; i < n; i++)
    ctpool_submit(pool, phase1_fn, &items[i], NULL);

/* Barrier: all phase 1 work is done before phase 2 starts */
ctpool_wait(pool);

/* Submit phase 2 tasks that depend on phase 1 results */
for (size_t i = 0; i < n; i++)
    ctpool_submit(pool, phase2_fn, &items[i], NULL);

ctpool_shutdown_drain(pool);
ctpool_destroy(pool);
```

No other thread should be submitting tasks concurrently when `ctpool_wait` is called. With an unbounded queue, concurrent submissions can cause indefinite blocking.

### Shutdown Modes

```c
/* Drain: finish all queued and active tasks, then stop workers */
ctpool_shutdown_drain(pool);
ctpool_destroy(pool);

/* Immediate: cancel queued tasks, stop after active tasks finish */
ctpool_shutdown_immediate(pool);
ctpool_destroy(pool);
```

Futures for tasks that were in the queue at the time of an immediate shutdown become cancelled:

```c
ctpool_future *f = ctpool_submit_future(pool, fn, arg);
ctpool_shutdown_immediate(pool);

void *result = ctpool_future_get(f);           /* returns NULL */
if (ctpool_future_cancelled(f)) { /* task never ran */ }
ctpool_future_free(f);
```

If neither shutdown function is called before `ctpool_destroy`, a drain shutdown is performed implicitly.

### Scoped Variant

```c
void process_batch(void) {
    ctpool_construct_scoped(pool, 4, 0);   /* unbounded queue */

    for (int i = 0; i < 1000; i++)
        ctpool_submit(pool, work_fn, items[i], NULL);

    ctpool_shutdown_drain(pool);
    /* pool is destroyed automatically when the function returns */
}
```

### Real-World Use Case: Counting Words Across a Book's Chapters in Parallel

A small writing tool counts the total number of words in a book, where each chapter is stored in its own text file. Counting each chapter is independent work, so it can run on a worker thread while a shared atomic counter accumulates the total. `ctpool_wait` acts as a barrier between the parallel counting phase and printing the final total:

```c
typedef struct { const char *path; atomic_int *word_count; } ChapterJob;

void count_words_in_chapter(void *arg) {
    ChapterJob *job = (ChapterJob *)arg;
    FILE *f = fopen(job->path, "r");
    if (!f) return;
    char word[64];
    while (fscanf(f, "%63s", word) == 1)
        atomic_fetch_add(job->word_count, 1);
    fclose(f);
}

void count_book_words(const char **chapter_paths, size_t count) {
    ctpool_construct_scoped(pool, 4, ccol_invalid_size);

    atomic_int total_words = 0;
    ChapterJob jobs[count];
    for (size_t i = 0; i < count; i++) {
        jobs[i] = (ChapterJob){ .path = chapter_paths[i], .word_count = &total_words };
        ctpool_submit(pool, count_words_in_chapter, &jobs[i], NULL);
    }

    ctpool_wait(pool);   /* every chapter has been counted */

    printf("total words in book: %d\n", atomic_load(&total_words));

    ctpool_shutdown_drain(pool);
}
```

The pool is created with `ccol_invalid_size` (unbounded queue) so that submitting all `count` jobs in the loop never blocks. `ctpool_wait` then acts as a join point before printing the final total.

### Reference: Core Operations

**Lifecycle**

| Function / Macro | Description |
|---|---|
| `ctpool_declare(name)` | Declare an uninitialized `ctpool` variable |
| `ctpool_declare_scoped(name)` | Declare with auto-cleanup via `__attribute__((cleanup(...)))`, without initializing |
| `ctpool_construct(name, num_threads, queue_cap)` | Declare and initialize in one step; calls `fatal_err()` on failure |
| `ctpool_construct_scoped(name, num_threads, queue_cap)` | Declare, initialize, and register auto-cleanup; calls `fatal_err()` on failure |
| `create_cthread_pool(num_threads, queue_cap, err_str)` | Allocate and return a pool using the default allocator; returns `CTPOOL_INVALID` on failure |
| `create_cthread_pool_mp(num_threads, queue_cap, mprocs, err_str)` | Allocate and return a pool with a custom allocator; returns `CTPOOL_INVALID` on failure |
| `ctpool_destroy(pool)` | Drain-shutdown if needed, free all resources, and set the handle to `CTPOOL_INVALID`; fatal on an already-destroyed/stale handle |

**Task Submission**

| Function | Description |
|---|---|
| `ctpool_submit(pool, fn, arg, on_complete)` | Submit a task; blocks if the bounded queue is full; `on_complete` may be `NULL` |
| `ctpool_try_submit(pool, fn, arg, on_complete)` | Non-blocking submit; returns `ccol_container_full` instead of blocking |
| `ctpool_timed_submit(pool, fn, arg, on_complete, timeout)` | Timed submit; `timeout` is a relative `struct timespec`; `NULL` behaves as try-only |

**Futures**

| Function | Description |
|---|---|
| `ctpool_submit_future(pool, fn, arg)` | Submit a task returning `void *`; returns a future or `NULL` on OOM or shutdown |
| `ctpool_future_get(f)` | Block until the result is ready and return it; returns `NULL` if cancelled |
| `ctpool_future_done(f)` | Non-blocking poll: `true` if the result is ready or the future was cancelled |
| `ctpool_future_cancelled(f)` | `true` if the task was discarded by `ctpool_shutdown_immediate` |
| `ctpool_future_free(f)` | Release the caller's reference; must be called exactly once per `ctpool_submit_future` |
| `ctpool_future_create_detached(err_str)` | Create a standalone future with no pool/task, to be fulfilled by an external producer |
| `ctpool_future_fulfill(f, result)` | Deliver a result to a detached future; `ccol_not_permitted` if already fulfilled |

**Management**

| Function | Description |
|---|---|
| `ctpool_wait(pool)` | Block until the queue is empty and all active tasks have completed |
| `ctpool_shutdown_drain(pool)` | Graceful shutdown: finish all queued tasks, then stop workers |
| `ctpool_shutdown_immediate(pool)` | Immediate shutdown: discard queued tasks, stop after active tasks finish |
| `ctpool_pending_count(pool)` | Number of tasks currently in the queue (not yet picked up by a worker) |
| `ctpool_active_count(pool)` | Number of tasks currently being executed by worker threads |

---

## 18. HTTP Client - `chttpclient`

`chttpclient` lets your C program send HTTP requests (GET, POST, PUT, DELETE, PATCH) to any URL and receive the response. It is a hand-rolled HTTP/1.1 client: an internal `chttp1_parser` module drives request/response framing over raw sockets, TLS is provided by `ctls` (a reactor-agnostic OpenSSL wrapper), the reactor backing Tier 2/3's async engine is `event_loop` (from `cthreadcomm`), and this module adds a concurrency-limiting pool, a keep-alive connection cache, case-insensitive header maps, and an API that integrates with the rest of the library. `chttpclient` has no dependency on any vendored third-party code.

`chttpcli` is an opaque VALUE handle, not a pointer: it must never be cast to/from `void *`, compared via a pointer cast, or treated as an address. Compare it against `CHTTPCLI_INVALID` (or use a truthiness check; `CHTTPCLI_INVALID` is `0`, so `if (!cli)` works as expected). Internally, every use of a `chttpcli` is resolved through a library-owned slot table before the underlying client object is touched, so a stale handle (one whose client has already been destroyed) is always detected rather than silently dereferencing freed memory.

**Supported URL forms:** `http://`/`https://`, plus `http+unix://<percent-encoded-socket-path>[/path][?query]` for connecting to a server listening on a Unix domain socket (matching Python's `requests-unixsocket` convention; `https+unix://` is not supported). Both a plain hostname/IPv4 literal and a bracketed IPv6 literal (`https://[::1]:8443/path`) are accepted for the network forms. A URL may embed credentials (`http://user:pass@host/path`); they are turned into an `Authorization: Basic ...` header automatically unless the request already sets its own `Authorization` header (not supported for `http+unix://`, which has no established userinfo convention). A trailing `#fragment` is recognized and discarded (fragments are a client-side-only concept and are never sent to a server). See "Redirect Following" below for how embedded credentials interact with redirects to a different origin, and "Unix Domain Sockets" below for the `http+unix://` scheme in full.

The module is split across two headers: `chttp.h` declares shared types (`chttp_method_t`, `chttp_tls_config_t`, `chttp_request_body_t`, and status-code constants), and `chttpclient.h` declares the client API. Including `chttpclient.h` pulls in `chttp.h` automatically.

**Header:** `#include <chttpclient.h>`

**Link with:** `-lssl -lcrypto -lm` (TLS support; no other external dependency)

### Convenience API - One-Liner Requests

The simplest path uses the process-level default client via the `chttp_get`, `chttp_post`, `chttp_put`, `chttp_delete`, and `chttp_patch` convenience functions. The default client is lazily initialized on the first call, uses the CPU count as the pool size, and has TLS peer and host verification enabled.

Do not pass the handle returned by `chttp_default_client()` to `chttpclient_destroy`: it is owned by the library, which destroys it automatically at process exit. Doing so anyway will not crash that specific call, but the default client is never rebuilt afterward, so every later call to `chttp_default_client()` or any of the convenience functions above fails cleanly for the remainder of the process. If you need a client with a lifetime you control, create your own with `create_chttpclient`/`create_chttpclient_mp` instead.

Every `resp_out`-taking entry point (`chttpclient_do`, `chttpclient_do_pooled`, `chttp_do`, `chttp_run_query`, and the convenience functions above) sets `*resp_out` to `NULL` immediately, before any other work begins, and leaves it `NULL` on every non-success return; combined with `chttpclient_resp_free`'s own "safe to call with NULL" contract, this means it is always safe to call `chttpclient_resp_free(resp)` unconditionally after one of these calls, regardless of the returned `ccol_retval_t`, without separately pre-initializing your own local pointer:

```c
chttpcli_response *resp = NULL;

if (chttp_get("https://api.example.com/users", &resp) == ccol_success) {
    printf("status=%d\n", resp->status_code);
    printf("body=%s\n",   resp->body);
    chttpclient_resp_free(resp);
}
```

For methods that carry a body, use the body macros from `chttp.h`:

```c
const char *payload = "{\"name\":\"alice\"}";
chttpcli_response *resp = NULL;

chttp_post("https://api.example.com/users",
           &CHTTP_JSON_BODY(payload, strlen(payload)),
           &resp);
if (resp) {
    printf("created: %d\n", resp->status_code);
    chttpclient_resp_free(resp);
}
```

### Custom Client

When you need to control pool size, timeouts, or TLS behaviour, create a dedicated `chttpcli` handle:

```c
chttpcli_construct(cli);

chttpclient_set_pool_size(cli, 8);
chttpclient_set_connect_timeout(cli, 2000);   /* 2 s TCP connect timeout */
chttpclient_set_request_timeout(cli, 10000);  /* 10 s total request timeout */
chttpclient_set_max_response_body_size(cli, 10 * 1024 * 1024); /* 10 MiB cap */

chttp_request_t *req = chttp_request_new(CHTTP_POST,
    "https://api.example.com/items",
    &CHTTP_JSON_BODY(json_str, json_len),
    NULL);
chttp_request_set_header(req, "Authorization", "Bearer my-token");

chttpcli_response *resp = NULL;
ccol_retval_t rc = chttpclient_do(cli, req, &resp);
chttp_request_free(req);

if (rc == ccol_success) {
    printf("%d: %s\n", resp->status_code, resp->body);
    chttpclient_resp_free(resp);
}

chttpclient_destroy(cli);
```

Calling `chttpclient_destroy`/`__chttpclient_destroy` again on a handle that has already been destroyed (whether sequentially, well after the first call completed, or concurrently, racing it from another thread) is a fatal error (`fatal_err()`, `abort()`/`SIGABRT`), not a silent double-free; see `chttpclient_destroy(3)` for the full contract.

`chttpclient_do` blocks until a pool slot is free, executes the request synchronously, and returns the fully buffered response. Multiple threads may call `chttpclient_do` concurrently on the same handle.

### Request and Response Headers

Header names are normalised to lowercase on storage; all lookups are therefore case-insensitive:

```c
chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
chttp_request_set_header(req, "Accept",     "application/json");
chttp_request_set_header(req, "X-Trace-Id", trace_id);

/* Read back - case-insensitive */
const char *accept = chttp_request_get_header(req, "accept");
```

Response headers follow the same convention:

```c
const char *ct = chttpclient_resp_header(resp, "content-type");
```

To iterate all response headers:

```c
ccol_for_each(resp->headers, it) {
    printf("key-name: %s -> key-value: %s\n",
        *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
}
```

Every header this module auto-injects a default value for (Host, Accept,
User-Agent, Content-Length, Content-Type, Authorization, Expect) is detected
in a case-insensitive manner, so setting any of them explicitly (via
`chttp_request_set_header`, or via `chttp_run_query`'s `headers` map)
suppresses the corresponding auto-injected default and the caller's own
value is sent on the wire exactly as given, including a custom Host header.

A `headers` map handed to `chttp_run_query` directly is keyed by exact byte
content, so it may legally contain two literally different keys that are the
same header name under HTTP's case-insensitive semantics (e.g. `"Host"` and
`"host"` both present at once); `chttp_request_set_header` cannot produce
this on its own, since it lower-cases every key before storing it. Request
serialization deduplicates such case-variant duplicates before writing them
onto the wire: only the most recently inserted one is sent, never both, so
two `Host:` lines (a request RFC 7230 SS5.4 requires a server to reject
outright) can never reach the wire this way.

A header value may not contain an embedded CR or LF byte, and a header
name must be a non-empty RFC 7230 tchar-only token (letters, digits, and
`!#$%&'*+-.^_`|~`): `chttp_request_set_header` rejects a call violating
either rule with `ccol_invalid_args` before storing anything, and the
same checks run again as a backstop at request-serialization time
(covering a `headers` map handed to `chttp_run_query` directly, bypassing
`chttp_request_set_header` entirely) so a header value built from
untrusted data (a forwarded token, a proxied header) can never split the
request or inject extra header lines into it, and a malformed name (a
space or literal `:`, say) can never reach the wire either.

A "Transfer-Encoding" header name is likewise always rejected (again with
the identical set-time-plus-serialization-time-backstop treatment): this
client never transfer-codes a request body, so honoring a caller-set
Transfer-Encoding header is impossible, and letting one reach the wire
would pair it with this module's own auto-synthesized Content-Length
header over a body that was never actually transfer-coded, an ambiguous
framing this library's own request/response parser explicitly rejects
when receiving one.

A caller-set "Content-Length" header IS accepted (unlike Transfer-Encoding,
`chttp_request_set_header` has no reason to reject it outright), but on a
body-carrying request (POST, PUT, or PATCH) it is validated at
request-serialization time against the body actually being sent, once
the request reaches the wire: a value that is not a plain unsigned decimal
digit string (no leading `+`/`-`, no embedded whitespace) exactly matching
the real body length is rejected with `ccol_invalid_args`, for the
identical "declared framing disagrees with the wire" reason
Transfer-Encoding is rejected outright above, just reached through a wrong
or malformed length instead of a wrong transfer-coding. This is only
checked for a body-carrying
request; for any other method (GET, DELETE, HEAD, OPTIONS) a
Content-Length header is stripped from the wire entirely (there is no
body to describe), so there is no framing left for a mismatched value
to desync from.

### Streaming Response

When the response body is large or must be processed incrementally, use `chttpclient_do_streaming`. The write callback receives chunks as they arrive:

```c
size_t write_to_file(const void *data, size_t len, void *ctx) {
    return fwrite(data, 1, len, (FILE *)ctx);
}

FILE *out = fopen("response.bin", "wb");
int status_code = 0;

chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
chttpclient_do_streaming(cli, req, write_to_file, out, &status_code);
chttp_request_free(req);
fclose(out);
```

Response headers are not accessible via the streaming path. Returning a value less than `len` from the write callback aborts the transfer.

### Interim (1xx) Responses

Any interim informational (1xx) response a server sends is transparently discarded, with reading continuing on the same connection until the real response arrives; no tier ever hands a 1xx status back to the caller as though it were the final answer. RFC 8297 `103 Early Hints` is the most common real-world example, and may precede an ordinary response with no `Expect: 100-continue` involvement at all. This applies uniformly to `chttpclient_do`/`chttpclient_do_streaming` (Tier 1) and `chttpclient_do_async`/`chttpclient_do_async_streaming` and everything built on them (Tiers 2/3). A server that never stops sending interim responses is not tolerated indefinitely: after 64 consecutive discarded interim responses on one hop, the client gives up and reports `ccol_http_transfer_aborted` rather than letting a misbehaving or malicious server pin a caller thread (Tier 1) or a chain/future (Tier 2/3) forever, particularly relevant given `request_timeout_ms` defaults to 0 (no timeout).

### Expect: 100-continue

Set `req->expect_continue = true` on a POST/PUT/PATCH request with a body to hold the body back until the server confirms it wants it, avoiding wasted upload bandwidth against a server that is about to reject the request outright (e.g. on size or authentication grounds):

```c
chttp_request_t *req = chttp_request_new(CHTTP_POST,
    "https://api.example.com/uploads",
    &CHTTP_JSON_BODY(json_str, json_len),
    NULL);
req->expect_continue = true;

chttpcli_response *resp = NULL;
ccol_retval_t rc = chttpclient_do(cli, req, &resp);
chttp_request_free(req);
```

The client sends only the request's headers first, then waits up to one second for the server's interim `100 Continue` response before sending the body; any OTHER interim 1xx status the server sends while waiting (e.g. `103 Early Hints` ahead of `100 Continue`) is discarded and the wait continues, still bounded by the same one-second budget. If the server answers with a final status directly instead (e.g. `417 Expectation Failed`), that response is delivered to the caller and the body is never sent. A server that never replies within the one-second window is assumed to simply not support the mechanism: the body is sent anyway and the request proceeds normally. This has no effect on a request with no body. Every tier honors `expect_continue` identically: `chttpclient_do`/`chttpclient_do_streaming` (Tier 1) implement the wait as a blocking call; `chttpclient_do_async`/`chttpclient_do_async_streaming` and everything built on them (Tiers 2/3) implement the identical three-outcome protocol as part of their own non-blocking, event-driven dispatch. The 64-consecutive-discarded-interim-response cap described above applies independently to this wait and to the final response read that follows it: a "100 Continue" (or the wait simply timing out) is itself a genuine, non-discarded outcome, so it resets the count; a request using `expect_continue` may therefore discard up to 64 interim responses while waiting for "100 Continue" and up to 64 more while reading the final response, not merely 64 combined.

Once the server has sent `100 Continue` and the body has been written to the connection, that connection is never silently retried again for this request: see the reused-connection retry note under "Keep-alive idle pool" below for why an explicit `100 Continue` permanently disqualifies a hop from the reused-connection retry-once safety net, even if the connection then dies before the real final response arrives.

### Asynchronous Requests (Tier 2)

`chttpclient_do` and `chttpclient_do_streaming` are synchronous; each call blocks the calling thread for the duration of the request. `chttpclient_do_async` and `chttpclient_do_async_streaming` submit a request to a shared, lazily-started reactor engine and return immediately with a `ctpool_future *`:

```c
chttpcli_construct(cli);

chttp_request_t *req = chttp_request_new(CHTTP_GET,
    "https://api.example.com/items", NULL, NULL);
ctpool_future *f = chttpclient_do_async(cli, req);
chttp_request_free(req); /* req need not outlive this call, unlike chttpclient_do */

/* ... do other work while the request is in flight ... */

chttpcli_async_result_t *result = chttpclient_async_result_get(f); /* blocks */
if (result->rv == ccol_success) {
    printf("%d: %s\n", result->resp->status_code, result->resp->body);
    chttpclient_resp_free(result->resp);
}
chttpclient_async_result_free(result);
ctpool_future_free(f);

chttpclient_destroy(cli);
```

The engine (a small pool of `event_loop` reactor threads plus a companion DNS/connect worker pool, both sized to the CPU count by default) starts on the first call to `chttpclient_do_async`/`_streaming` anywhere in the process and stops automatically once no request is in flight and no connection remains pooled; it is entirely independent of `chttpclient_do`'s synchronous connection handling. `chttpclient_destroy` blocks until every Tier 2/3 request still in flight for that client completes, exactly like it already does for Tier 1; it is safe to call even if a future returned by `chttpclient_do_async`/`_streaming` has not been waited on yet, though the future itself remains valid to use afterward (its result was already available by the time `chttpclient_destroy` returned). This engine owns its own static, process-wide `event_loop` instance, fully independent of `chttpserver`'s own (separate) `event_loop` instance; the two modules share no reactor, so stopping/starting one has no effect on the other. `req` is fully copied/serialised before `chttpclient_do_async`/`_streaming` returns, so (unlike `chttpclient_do`) it never needs to outlive the call. `connect_timeout_ms`/`request_timeout_ms` (set via `chttpclient_set_connect_timeout`/`chttpclient_set_request_timeout`) and keep-alive connection reuse both apply identically to Tier 2 as they do to `chttpclient_do`. `chttpcli_set_engine_logger`/`chttpcli_set_engine_mem_mgmt_procs`/`chttpcli_set_engine_num_reactor_threads` configure this reactor's diagnostics logger, allocator, and OS thread count respectively, and must be called before this engine's first lazy construction (mirroring `chttpserver`'s identical trio of functions for its own reactor).

`chttpclient_do_async_streaming` delivers the response body via a `chttpcli_write_fn` callback, exactly like `chttpclient_do_streaming`:

```c
ctpool_future *f = chttpclient_do_async_streaming(cli, req, write_to_file, out);
```

The callback runs on one of the engine's own reactor threads; **not** the calling thread. It must not block (no blocking I/O, no long-held locks) and must not call back into `chttpclient_do_async`/`_streaming` for any client sharing the engine, since doing so risks deadlocking against the very reactor thread it runs on. As with the synchronous streaming path, a return value less than `len` aborts the transfer (`ccol_http_transfer_aborted`), and response headers are not accessible.

`chttpcli_async_result_t` (`rv`, `resp`) is obtained via `chttpclient_async_result_get` (a typed wrapper over `ctpool_future_get`) and released via `chttpclient_async_result_free`; do this before `ctpool_future_free`. `resp` is non-NULL only when `rv == ccol_success`; for the streaming variant, `resp` is still populated (so `status_code` is available) but `resp->body` stays NULL, matching `chttpclient_do_streaming`'s own convention.

### Pooled-Sync Requests (Tier 3)

`chttpclient_do_pooled` and `chttpclient_do_pooled_streaming` are thin blocking wrappers over Tier 2: they submit the request to the shared reactor engine and block until it completes, but return the exact same `ccol_retval_t`/`resp_out` (or `status_code_out`) call shape `chttpclient_do`/`chttpclient_do_streaming` use; no future, no result struct to manage:

```c
chttpcli_response *resp = NULL;
ccol_retval_t rc = chttpclient_do_pooled(cli, req, &resp);
if (rc == ccol_success) {
    printf("%d: %s\n", resp->status_code, resp->body);
    chttpclient_resp_free(resp);
}
```

This gives blocking-call ergonomics while sharing the engine's small, fixed-size reactor thread pool across every concurrent caller, instead of each call blocking its own OS thread the way `chttpclient_do` does. Error codes match `chttpclient_do` exactly for everything detected once the request is in flight (bad URL, TLS failure, connection failure, transfer errors, timeouts, too many redirects); a failure to even submit the request to the engine (OOM, or the engine failing to start) is reported as `ccol_unexpected_failure` rather than a more specific code. `chttpclient_do_pooled_streaming` mirrors `chttpclient_do_streaming`'s signature and semantics, with the same reactor-thread callback contract `chttpclient_do_async_streaming` documents.

To redirect the engine's own diagnostics (TLS handshake failures, connect errors) to a `clog` handle, call `chttpcli_set_engine_logger` before the first Tier 2/3 call anywhere in the process:

```c
clog logger = clog_open_fd(2, CLOG_INFO, NULL);
chttpcli_set_engine_logger(logger);   /* derive engine sub-logger; optional */
```

If no logger is ever configured, a fallback logger (fd 2, level `CLOG_FATAL`) is installed automatically the first time the engine starts; since the engine's own diagnostics are never logged above `CLOG_INFO`, that fallback logger is silent in practice unless `chttpcli_set_engine_logger` is used to install a more verbose one. To redirect the engine's own internal memory management (the `event_loop` instance itself, its DNS/connect worker pool, and its own connection-registration bookkeeping) to a custom allocator, call `chttpcli_set_engine_mem_mgmt_procs` before the engine's first start (or after it has fully stopped):

```c
ccol_memmgmt_procs_t mp = {
    .malloc = my_malloc, .free = my_free,
    .calloc = my_calloc, .realloc = my_realloc,
};
chttpcli_set_engine_mem_mgmt_procs(&mp);   /* optional; NULL reverts to default */
```

This is independent of the allocator each individual `chttpcli` instance uses for its own requests/connections (configured via `create_chttpclient_mp`); this setter only affects the one shared engine's own construction. To override how many OS threads the shared reactor devotes to its own polling and dispatch (by default it auto-detects `sysconf(_SC_NPROCESSORS_ONLN)`, falling back to 1), call `chttpcli_set_engine_num_reactor_threads` under the same "before first start, or after a full stop" restriction:

```c
chttpcli_set_engine_num_reactor_threads(4);   /* optional; 0 restores auto-detected sizing */
```

All three functions mirror `chttpserver`'s identical `chttpsvr_set_engine_logger`/`chttpsvr_set_engine_mem_mgmt_procs`/`chttpsvr_set_engine_num_reactor_threads` trio for its own, fully independent reactor.

### TLS Configuration

By default, both peer and host verification are on and the system CA bundle is used. Override with `chttpclient_set_tls`:

```c
/* Disable verification (development / self-signed certs only) */
chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
tls.verify_peer = false;
tls.verify_host = false;
chttpclient_set_tls(cli, &tls);

/* Custom CA bundle */
chttp_tls_config_t tls2 = CHTTP_TLS_DEFAULT;
tls2.ca_bundle_path = "/etc/myapp/ca-chain.pem";
chttpclient_set_tls(cli, &tls2);
```

Pass `NULL` to restore the defaults.

`cert_path` and `key_path` are a pair: providing exactly one of the two (the other left `NULL`) is rejected outright with `ccol_invalid_args`, rather than silently treated as "no client certificate configured" (which would leave an mTLS deployment believing it presents a client certificate when it never does). Both paths are validated for readability lazily, at the time an HTTPS request actually needs them, not by `chttpclient_set_tls` itself; a path that does not currently exist is accepted without error at configuration time and only surfaces as `ccol_http_tls_cert_load_failed` from `chttpclient_do`/`chttpclient_do_streaming` once a request needs it.

`verify_host` always implies `verify_peer` in practice: hostname matching against a certificate whose chain was never validated gives no real security guarantee, since the certificate itself could be entirely forged. Setting `verify_peer = false, verify_host = true` does not get you "hostname-only checking with no chain trust"; it gets full verification (using the system CA store, or `ca_bundle_path` if set), the same as `verify_peer = true` would. To genuinely disable all server certificate checking, set both `verify_peer = false` and `verify_host = false`, as in the example above.

Firing many concurrent HTTPS requests through Tier 2/3 (`chttpclient_do_async`/`_pooled`) to certificate-verifying origins that share one `chttpcli`'s trust store can, under ThreadSanitizer, show a race inside OpenSSL's own certificate-comparison internals (`X509_NAME_cmp`/`X509_cmp`'s canonical-encoding lazy cache) rather than in anything this library controls; every per-connection OpenSSL object this library allocates is independent per connection, and building the shared TLS context itself is already mutex-protected. This is a known, version-spanning class of issue in OpenSSL's own issue tracker, not something a caller can work around from the outside; it does not affect the outcome of any handshake. No functional workaround is applied: doing so would mean serialising concurrent handshakes, defeating the point of a reactor built to multiplex several of them at once, to compensate for a bug in a dependency outside this library's control.

### Connection Pool Behaviour

Each `chttpcli` handle has two independent layers:

- **Concurrency limiter**; bounds the number of simultaneous in-flight requests. The limit defaults to the CPU count (`chttpclient_set_pool_size`); when the limit is reached, `chttpclient_do` blocks until a slot frees up, providing natural backpressure with no external semaphore required.
- **Keep-alive idle pool**; after a request completes on an HTTP/1.1 keep-alive connection, the connection (and, for HTTPS, its already-established TLS session) is kept open and cached per origin (scheme + host + port) so a later request to the same origin can skip DNS resolution, the TCP handshake, and the TLS handshake entirely. A cheap liveness probe runs before reuse; a connection the peer has since closed is transparently discarded and replaced with a fresh one. Because the probe and the actual request are not atomic, the peer can still close the connection in between; if that happens, the request is transparently retried exactly once against a brand-new connection; this covers both a failed write and a failed (or empty) read, as long as no response bytes have been parsed or handed back to the caller yet, so nothing is ever silently duplicated. The one exception is a request using `Expect: 100-continue` (see that section above; applies uniformly across every tier): once the server has explicitly confirmed readiness with `100 Continue` and the body has been written to the connection, that hop is no longer eligible for this retry, since the server has already proven it was alive and accepted the body, retrying would resend that body to an unrelated second connection, and a non-idempotent request could then be processed twice; a subsequent failure on that hop is reported to the caller instead. Idle connections are bounded per origin and in total, and expire after a short idle period; once the caps are hit, a completed connection is simply closed instead of cached, which only forfeits the reuse optimisation and never affects correctness.

Buffered (non-streaming) response bodies have no size limit by default: `chttpclient_do`, `chttpclient_do_async` (and the pooled-sync wrappers built on it), and the `chttp_get`/`post`/`put`/`delete`/`patch`/`run_query` convenience wrappers will all buffer an entire response body into memory regardless of size. `chttpclient_set_max_response_body_size(cli, max_bytes)` caps this (`0`, the default, means unlimited): a response whose `Content-Length` alone already declares more than `max_bytes` is rejected immediately, before any body byte is read off the wire; a chunked or connection-close-delimited body (no declared length to check up front) is instead rejected the moment the bytes actually received so far would exceed `max_bytes`. Either case reports `ccol_msg_too_large` and the connection is not reused afterward. This cap has no effect on `chttpclient_do_streaming`/`chttpclient_do_async_streaming`/`chttpclient_do_pooled_streaming`: a streaming caller already controls its own memory via its `chttpcli_write_fn`'s return value. The up-front declared-length check only ever applies to the message that will actually deliver the caller's body: an intermediate redirect hop's own body is always exempt (its body is discarded regardless of length; see "Redirect Following" below), a discarded 1xx informational response's declared `Content-Length` (RFC 7230 SS3.3.2 says a compliant server should never send one, but a misbehaving or malicious server might) is likewise exempt since that response is never delivered to the caller, and a `HEAD` response's `Content-Length` (which describes what a `GET` would have returned, per RFC 7231 SS4.3.2, and is never followed by actual body bytes) is exempt as well.

### Redirect Following

Redirects (`chttpclient_do` and `chttpclient_do_streaming` both follow up to 50 hops) apply an explicit method/body policy on each hop: 301, 302, and 303 rewrite the method to a bodyless GET (HEAD is left as HEAD), while 307 and 308 preserve the original method and resend the original body. If the 50-hop cap is reached and the last hop's response is itself a would-be redirect, it is not followed or delivered; `ccol_http_too_many_redirects` is returned instead.

A `Location` header may be an absolute URL, a protocol-relative reference (`//host/path`), an absolute-path reference (`/foo`), or a general relative reference (`foo`, `../foo`, `./foo`, `?query`); all are resolved per RFC 3986. A `Location` value that carries its own scheme (e.g. `mailto:x@y`, `ftp://host/path`, or any scheme other than `http`, `https`, or `http+unix`) is always treated as absolute (RFC 3986 SS5.2.2: a reference with a scheme is never relative, regardless of whether that scheme is one this client can actually fetch) and is resolved to itself unchanged; since this client only ever connects over `http://`/`https://`/`http+unix://`, the next hop then reports `ccol_http_invalid_url`, the same code an unsupported scheme in the original request URL already gets, rather than the reference being silently merged onto the current origin's path as though it were relative. Dot-segment normalization (`..`, `.`) only ever rewrites the path component; a query string is always carried forward byte-for-byte, even one that happens to contain `/`, `..`, or `.` characters. If the original request URL embedded credentials, the resulting `Authorization: Basic ...` header is resent on every subsequent hop as long as the redirect stays on the same origin (scheme + host + port); it is dropped permanently (and never re-acquired even if a later hop redirects back to the original origin) the first time a hop changes origin. This matches curl's own default behavior (without opting into trusted-redirect credential forwarding) and prevents credentials from leaking to an unexpected host via a redirect. A caller-supplied `Authorization` header set explicitly via `chttp_request_set_header` receives the identical same-origin-carry/permanent-cross-origin-drop treatment (also matching curl's own hardened default), tracked against the ORIGINAL request's origin rather than any per-hop userinfo. A redirect that stays on a `http+unix://` origin (see below) resolves relative references against the same socket path; a redirect cannot cross between a network origin and a Unix-socket origin (there is no way to express that in a single `Location` header) and is followed only if the `Location` itself names the target scheme explicitly.

A 301/302/303 hop that rewrites the method to a bodyless GET also strips any caller-set `Content-Length`, `Content-Type`, or `Expect` header from that hop's own request (and every hop after it, unless a later 307/308 hop restores a real body): those three headers only ever describe a body, and once no hop can carry one, resending a stale value describing the ORIGINAL request's body onto a request with no body at all is never correct and can make a receiving server block waiting for a body that will never arrive. Every other caller-set header is resent unchanged on every hop, exactly as before.

### Unix Domain Sockets

`chttpclient` can connect to a server listening on a Unix domain socket instead of a TCP/IP address, using the `http+unix://` URL scheme (matching Python's `requests-unixsocket` convention; `chttpsvr_config_t.host = "unix://<path>"` is the server-side counterpart):

```c
/* Socket path "/var/run/app.sock", percent-encoded (every byte, including
 * '/', except RFC 3986 unreserved characters, must be %XX-escaped). */
chttpcli_response *resp = NULL;
chttp_get("http+unix://%2Fvar%2Frun%2Fapp.sock/api/users", &resp);
```

All three tiers (`chttpclient_do`, `chttpclient_do_async`/`_pooled`) support it identically to a network URL, including keep-alive connection pooling (Tier 2/3's idle pool keys pooled connections by a `"unix://<path>"` origin, distinct from any `"scheme://host:port"` origin). Since there is no real hostname for a Unix-socket target, the `Host:` header defaults to `localhost` (matching curl's `--unix-socket` behavior) unless the request sets its own `Host` header explicitly. `https+unix://` is not supported (TLS over a local socket has no real use case); `chttpclient_set_tls` has no effect on `http+unix://` requests. A socket path that does not fit in `sockaddr_un.sun_path` (108 bytes on Linux, including the terminating NUL) is rejected as `ccol_http_invalid_url` before any connection attempt.

### Scoped Variant

```c
void fetch_data(void) {
    chttpcli_construct_scoped(cli);
    chttpclient_set_request_timeout(cli, 5000);

    chttpcli_response *resp = NULL;
    chttp_get("https://api.example.com/data", &resp);
    if (resp) {
        process(resp->body);
        chttpclient_resp_free(resp);
    }
    /* cli is destroyed automatically when the function returns */
}
```

### Real-World Use Case: Checking If Your Favorite Websites Are Up

A small "is it down?" utility checks whether a handful of favorite websites are currently reachable, all at once instead of one after another, on a shared `chttpcli`. Each worker thread acquires a separate pool slot so the checks execute in parallel. The pool capacity acts as the concurrency cap:

```c
typedef struct {
    chttpcli cli;
    const char *url;
    chttpcli_response *resp;
    ccol_retval_t rc;
} CheckArg;

void *check_site(void *arg) {
    CheckArg *ca = (CheckArg *)arg;
    chttp_request_t *req = chttp_request_new(CHTTP_GET, ca->url, NULL, NULL);
    ca->rc = chttpclient_do(ca->cli, req, &ca->resp);
    chttp_request_free(req);
    return NULL;
}

void check_favorite_sites(chttpcli cli) {
    CheckArg sites[3] = {
        { cli, "https://www.example.com", NULL, ccol_success },
        { cli, "https://www.example.org", NULL, ccol_success },
        { cli, "https://www.example.net", NULL, ccol_success },
    };

    pthread_t threads[3];
    for (int i = 0; i < 3; i++)
        pthread_create(&threads[i], NULL, check_site, &sites[i]);
    for (int i = 0; i < 3; i++)
        pthread_join(threads[i], NULL);

    for (int i = 0; i < 3; i++) {
        if (sites[i].rc == ccol_success)
            printf("%s -> %d\n", sites[i].url, sites[i].resp->status_code);
        else
            printf("%s -> unreachable\n", sites[i].url);
        chttpclient_resp_free(sites[i].resp);
    }
}
```

If the pool size is smaller than the number of concurrent callers, excess threads block inside `chttpclient_do` until a slot is released, automatically bounding peak concurrency.

### Error Return Values

`chttpclient_do`, `chttpclient_do_streaming`, and the convenience wrappers return a `ccol_retval_t`. In addition to the generic codes shared by the rest of the library (`ccol_success`, `ccol_invalid_args`, `ccol_not_enough_memory`, `ccol_timed_out`, `ccol_not_permitted`), the HTTP client returns the following specific values:

| Return value | Meaning |
|---|---|
| `ccol_http_invalid_url` | URL is malformed, uses an unsupported scheme (only `http://`/`https://`/`http+unix://` are supported), has a missing/invalid host or port, has an embedded CR or LF byte in the host or path/query component (which would otherwise be carried verbatim onto the wire and let it inject extra header lines or a smuggled second request), or (for `http+unix://`) a socket path too long to fit `sockaddr_un.sun_path` |
| `ccol_http_host_resolution_failed` | DNS resolution failed for the target host (never returned for `http+unix://`, which has no DNS step) |
| `ccol_http_connection_failed` | The connection could not be established (e.g. connection refused, or a `http+unix://` socket path that does not exist) |
| `ccol_http_too_many_redirects` | The redirect chain exceeded 50 hops |
| `ccol_http_tls_handshake_failed` | The TLS handshake failed for a reason other than certificate verification |
| `ccol_http_tls_cert_verification_failed` | The peer certificate or hostname could not be verified |
| `ccol_http_tls_cert_load_failed` | The configured client certificate, key, or CA bundle path was not readable, or ctls failed to load/parse it |
| `ccol_http_transfer_aborted` | The connection failed mid-transfer, the server sent a malformed HTTP/1.1 response, or a streaming `chttpcli_write_fn` returned fewer bytes than it was given |
| `ccol_msg_too_large` | The response body exceeded `chttpclient_set_max_response_body_size`'s configured cap (never returned unless that cap has been set, and never returned for a streaming request) |
| `ccol_timed_out` | `chttpclient_set_connect_timeout` or `chttpclient_set_request_timeout` elapsed before the operation completed |
| `ccol_unexpected_failure` | Any other internal failure not covered above |

`ccol_retval_to_str()` from `common.h` returns a string literal for any of these codes.

### Status Code Constants

`chttp.h` defines constants for all commonly used HTTP status codes:

| Constant | Value |
|---|---|
| `CHTTP_STATUS_OK` | 200 |
| `CHTTP_STATUS_CREATED` | 201 |
| `CHTTP_STATUS_ACCEPTED` | 202 |
| `CHTTP_STATUS_NO_CONTENT` | 204 |
| `CHTTP_STATUS_BAD_REQUEST` | 400 |
| `CHTTP_STATUS_UNAUTHORIZED` | 401 |
| `CHTTP_STATUS_FORBIDDEN` | 403 |
| `CHTTP_STATUS_NOT_FOUND` | 404 |
| `CHTTP_STATUS_INTERNAL_ERROR` | 500 |
| `CHTTP_STATUS_SERVICE_UNAVAILABLE` | 503 |

See `chttp.h` for the complete list.

### Base64 and Basic Auth Helpers

`chttp.h`/`chttp.c` expose standalone base64 and HTTP Basic auth (RFC 7617) helpers, usable independently of `chttpclient`/`chttpserver`:

```c
#include <chttp.h>

/* Build a ready-to-send Authorization header VALUE (no "Authorization: " key part). */
ccol_scoped_ptr(auth, char);
auth = chttp_basic_auth("Aladdin", "open sesame");
/* auth == "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==" */
chttp_request_set_header(req, "authorization", auth);
// free(auth); - // Shouldn't be called, since auth is declared as a scoped pointer

/* Standalone base64 encode/decode of arbitrary binary data. */
size_t enc_len = 0;
char *enc = chttp_base64_encode(data, data_len, &enc_len);

size_t dec_len = 0;
void *dec = chttp_base64_decode(enc, &dec_len); /* dec_len, not strlen(), is authoritative */
free(enc); // As enc is a raw pointer, it should be freed
free(dec); // As dec is a raw pointer, it should be freed
```

`chttp_base64_decode`'s output is NUL-terminated as a convenience for text payloads, but the decoded bytes may legitimately contain embedded NULs; always trust the `out_len` out-parameter, never `strlen()`, to determine the real decoded size. Each function has an `_mp`-suffixed variant taking an explicit `ccol_memmgmt_procs_t *` for a custom allocator; the plain names shown above use the default `malloc`/`free`.

Internally, `chttpclient.c`'s own URL parser calls `chttp_basic_auth_mp` to turn a `user:pass@host` URL userinfo component into the `Authorization` header it auto-injects; these are not a separate, parallel implementation.

### Reference: Core Operations

**Request Lifecycle**

| Function | Description |
|---|---|
| `chttp_request_new(method, url, body, err)` | Allocate a request using the default allocator; copies the URL and body data |
| `chttp_request_new_mp(method, url, body, mprocs, err)` | Allocate a request with a custom allocator |
| `chttp_request_set_header(req, name, value)` | Set or replace a request header; name is normalised to lowercase |
| `chttp_request_get_header(req, name)` | Look up a request header (case-insensitive); returns NULL if absent |
| `chttp_request_free(req)` | Free the request and all owned resources; safe to call with NULL |

**Client Lifecycle**

| Function / Macro | Description |
|---|---|
| `chttpcli_construct(name)` | Declare and initialize a client; calls `fatal_err()` on failure |
| `chttpcli_construct_scoped(name)` | Declare, initialize, and auto-destroy on scope exit; calls `fatal_err()` on failure |
| `chttpcli_declare(name)` | Declare an uninitialized client variable |
| `chttpcli_declare_scoped(name)` | Declare with automatic destruction on scope exit, without initializing |
| `create_chttpclient(err)` | Allocate and return a client using the default allocator; returns `CHTTPCLI_INVALID` on failure |
| `create_chttpclient_mp(mprocs, err)` | Allocate and return a client with a custom allocator; returns `CHTTPCLI_INVALID` on failure |
| `chttpclient_destroy(cli)` | Block until all in-flight requests finish, then free the client and set `cli` to `CHTTPCLI_INVALID`. Fatal (`abort()`/`SIGABRT`) if `cli` is a stale or already-destroyed handle |

**Client Configuration**

| Function | Description |
|---|---|
| `chttpclient_set_pool_size(cli, n)` | Set the maximum concurrent in-flight requests; 0 selects the CPU count |
| `chttpclient_set_connect_timeout(cli, ms)` | TCP connect timeout in milliseconds; 0 = no limit |
| `chttpclient_set_request_timeout(cli, ms)` | Total request timeout in milliseconds (connect + transfer); 0 = no limit |
| `chttpclient_set_max_response_body_size(cli, max_bytes)` | Cap the buffered response body size across all three tiers; 0 = no limit (the default). No effect on the streaming variants |
| `chttpclient_set_tls(cli, tls)` | Override TLS settings; NULL restores verification-on defaults |

**Engine Configuration (Tier 2/3)**

| Function | Description |
|---|---|
| `chttpcli_set_engine_logger(cl)` | Derive an engine sub-logger from `cl` that receives the shared reactor's own diagnostics; call before the first Tier 2/3 use in the process |
| `chttpcli_set_engine_mem_mgmt_procs(mp)` | Redirect the shared reactor's own internal memory management to `mp`, or to the default allocator if `mp` is NULL; call before the engine's first start, or after it has fully stopped |
| `chttpcli_set_engine_num_reactor_threads(n)` | Pin the shared reactor to `n` OS threads, or restore auto-detected sizing (`sysconf(_SC_NPROCESSORS_ONLN)`, falling back to 1) if `n` is 0; call before the engine's first start, or after it has fully stopped |

**Request Execution**

| Function | Description |
|---|---|
| `chttpclient_do(cli, req, resp_out)` | Execute a request and buffer the full response body; blocks until a pool slot is free |
| `chttpclient_do_streaming(cli, req, write_fn, ctx, status_out)` | Execute a request and deliver the body via a streaming callback; response headers are not accessible |
| `chttpclient_do_async(cli, req)` | Submit a request to the shared reactor engine; returns a `ctpool_future *` immediately, buffered response |
| `chttpclient_do_async_streaming(cli, req, write_fn, ctx)` | Same as `chttpclient_do_async`, but delivers the body via a callback run on an engine reactor thread |
| `chttpclient_async_result_get(f)` | Block until `f` is fulfilled; returns the typed `chttpcli_async_result_t *` |
| `chttpclient_async_result_free(result)` | Free a `chttpcli_async_result_t`; does not free `result->resp` or the future itself |
| `chttpclient_do_pooled(cli, req, resp_out)` | Submit via the shared reactor engine and block until complete; same call shape as `chttpclient_do` |
| `chttpclient_do_pooled_streaming(cli, req, write_fn, ctx, status_out)` | Same as `chttpclient_do_pooled`, but delivers the body via a callback run on an engine reactor thread |

**Default Client and Convenience API**

| Function | Description |
|---|---|
| `chttp_default_client()` | Return the lazily initialized process-level default client; thread-safe |
| `chttp_do(req, resp_out)` | Execute a request using the default client |
| `chttp_get(url, resp_out)` | GET request via the default client |
| `chttp_post(url, body, resp_out)` | POST request via the default client |
| `chttp_put(url, body, resp_out)` | PUT request via the default client |
| `chttp_delete(url, resp_out)` | DELETE request via the default client |
| `chttp_patch(url, body, resp_out)` | PATCH request via the default client |

**Response API**

| Function | Description |
|---|---|
| `chttpclient_resp_header(resp, name)` | Look up a response header by name (case-insensitive); returns NULL if absent |
| `chttpclient_resp_free(resp)` | Free the response and all owned resources; safe to call with NULL |

**Body Macros (`chttp.h`)**

| Macro | Description |
|---|---|
| `CHTTP_NO_BODY` | Zero-initializer for a bodyless request |
| `CHTTP_BODY(data, len, ct)` | Inline body with explicit content type |
| `CHTTP_JSON_BODY(data, len)` | Inline JSON body; sets Content-Type to `application/json` |
| `CHTTP_TEXT_BODY(data, len)` | Inline plain-text body; sets Content-Type to `text/plain` |
| `CHTTP_FORM_BODY(data, len)` | Inline URL-encoded form body; sets Content-Type to `application/x-www-form-urlencoded` |

**Base64 and Basic Auth (`chttp.h`)**

| Function | Description |
|---|---|
| `chttp_base64_encode(data, len, out_len)` | Base64-encode a buffer using the default allocator; `out_len` (optional) receives the encoded length |
| `chttp_base64_encode_mp(mp, data, len, out_len)` | Same, with a custom allocator |
| `chttp_base64_decode(b64_input, out_len)` | Base64-decode a NUL-terminated string using the default allocator; `out_len` (optional) receives the decoded byte length |
| `chttp_base64_decode_mp(mp, b64_input, out_len)` | Same, with a custom allocator |
| `chttp_basic_auth(username, password)` | Build a `"Basic <base64(username:password)>"` header value using the default allocator |
| `chttp_basic_auth_mp(mp, username, password)` | Same, with a custom allocator |

---

## 19. HTTP Server - `chttpserver`

`chttpserver` is an embedded HTTP/1.1 server. It provides a Go-style routing API: register handlers for method+pattern pairs, attach middleware chains, and create sub-routers with their own prefix and middleware. Multiple server instances may run simultaneously on different ports (or Unix domain sockets) within the same process, all sharing one process-wide reactor.

The module is built entirely on c_collections' own primitives rather than a vendored networking library: a multi-threaded `event_loop` (from `cthreadcomm`) drives the reactor, `ctls` provides a reactor-agnostic OpenSSL wrapper for TLS, and `chttp1_parser` is a small, hand-written HTTP/1.1 request parser. Routing happens on the reactor thread as soon as headers are parsed; the connection is then handed off to the server's own `ctpool` worker pool, which reads the body and runs the handler, so a large or slow body never blocks the reactor.

The module is split across two headers: `chttp.h` declares shared types (`chttp_method_t`, `chttp_tls_config_t`, status-code constants, and body macros), and `chttpserver.h` declares the server API. Including `chttpserver.h` pulls in `chttp.h` automatically.

**Header:** `#include <chttpserver.h>`

**Dependencies:** links `-lssl -lcrypto -lm -lpthread`

### Engine Lifecycle

`chttpserver` maintains one static, process-wide `event_loop` reactor (a single dedicated thread by default; see `chttpsvr_set_engine_num_reactor_threads` below to configure more) shared by every `chttpsvr` instance in the process, plus one idle-connection-timeout sweep thread shared the same way. Both are lazily started on the first `chttpsvr_start` call and torn down once the last server releases its reference (i.e. every started `chttpsvr` has been destroyed); no explicit engine start or stop call is required for ordinary use. That stop is asynchronous: destroying the last server does not itself guarantee the reactor has fully stopped by the time the destroy call returns. Call `chttpsvr_engine_wait()` afterward when a synchronous guarantee is needed (e.g. immediately reusing the port a just-destroyed server was listening on).

This reactor is entirely independent of `chttpclient`'s own engine (`chttpclient_do_async`/`_streaming`/`chttpclient_do_pooled`/`_streaming`); each module owns its own reactor, so stopping one never affects the other.

The library does not install any signal handlers. Applications are responsible for wiring shutdown into whatever signal or lifecycle mechanism they use. `chttpsvr_engine_stop()` is async-signal-safe and is the intended shutdown hook:

```c
#include <signal.h>
#include <chttpserver.h>

static void _on_signal(int sig) { (void)sig; chttpsvr_engine_stop(); }

int main(void) {
    signal(SIGINT,  _on_signal);
    signal(SIGTERM, _on_signal);

    /* ... create server, register routes, then start: */
    chttpsvr_start(srv, &cfg);   /* blocks until engine is up and port is bound */

    chttpsvr_engine_wait();      /* block until engine exits */
    chttpsvr_destroy(srv);       /* releases this server's engine reference */
}
```

`chttpsvr_engine_stop()` tears down this module's own shared reactor and idle-sweep thread only; it has no effect on `chttpclient`'s independent engine, and vice versa.

To redirect the reactor's own diagnostics (TLS handshake failures, listen-socket bind failures, idle-timeout closures) to a `clog` handle, call `chttpsvr_set_engine_logger` at any time (it takes effect immediately, and again after any full stop/restart cycle):

```c
clog logger = clog_open_fd(2, CLOG_INFO, NULL);
chttpsvr_set_engine_logger(logger);   /* derive engine sub-logger; optional */
```

If no logger is ever configured, a fallback logger (fd 2, level `CLOG_FATAL`) is installed automatically the first time the engine starts, mirroring `create_chttpsvr`'s own internal stderr/FATAL-only logger; since the engine's own diagnostics (idle-timeout closures, TLS handshake failures, listen-socket setup failures) are all logged below `CLOG_FATAL`, that fallback logger's `min_level` filters every one of them out, so it is silent in practice unless `chttpsvr_set_engine_logger` is used to install a more verbose one.

To redirect the reactor's own internal memory management (the `event_loop` instance itself, and its own connection-registration bookkeeping) to a custom allocator, call `chttpsvr_set_engine_mem_mgmt_procs` before the first `chttpsvr_start`:

```c
ccol_memmgmt_procs_t mp = {
    .malloc = my_malloc, .free = my_free,
    .calloc = my_calloc, .realloc = my_realloc,
};
chttpsvr_set_engine_mem_mgmt_procs(&mp);   /* optional; NULL reverts to default */
```

This may only be called before the first `chttpsvr_start` in the process (it returns `ccol_not_permitted` afterward): swapping allocators once the reactor has already allocated memory with the previous one would produce mismatched malloc/free pairs. Passing NULL later (also before the first start, or after the reactor has fully stopped) reverts to the default allocator. Note this is independent of the allocator each individual `chttpsvr` instance uses for its own connections/requests (configured via `create_chttpsvr_mp`, following the usual `_mp` convention); this setter only affects the one shared reactor's own construction.

By default the reactor uses exactly 1 thread: a single dedicated thread that both polls and dispatches every callback inline. This is faster and more latency-consistent than multiple dispatch threads for both plain HTTP and TLS-with-connection-reuse traffic (the common case for a well-behaved client population). Multiple dispatch threads only pull ahead under sustained *connection churn* combined with TLS (many distinct clients each opening a connection for only one or a few requests, so a large fraction of traffic pays a fresh handshake's CPU cost instead of amortizing it away); real for some deployments (a public API absorbing many one-off anonymous clients, an IoT/device gateway with frequent reconnects, a webhook receiver) but not the typical shape, since most HTTP client software pools and reuses connections specifically to avoid this cost. See `chttpsvr_set_engine_num_reactor_threads(3)` for the full breakdown by traffic shape. To raise the thread count for a deployment that knows its own traffic is churn-heavy, call it under the same "before the first `chttpsvr_start`, or after a full stop" restriction as the allocator setter above:

```c
chttpsvr_set_engine_num_reactor_threads(4);   /* optional; 0 restores the default (1) */
```

### Quick Start

```c
#include <signal.h>
#include <chttpserver.h>
#include <clogger.h>

static void hello(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    (void)req; (void)ctx;
    chttpsvr_resp_write_str(resp, "Hello, world!");
}

static void _on_signal(int sig) { (void)sig; chttpsvr_engine_stop(); }

int main(void) {
    signal(SIGINT,  _on_signal);
    signal(SIGTERM, _on_signal);

    clog logger = clog_open_fd(2, CLOG_INFO, NULL);
    chttpsvr_set_engine_logger(logger);   /* optional: route engine logs to logger */

    chttpsvr srv = create_chttpsvr(logger, NULL);

    chttpsvr_register_handler(srv, CHTTP_GET, "/hello", hello, NULL);

    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.port = 8080;
    chttpsvr_start(srv, &cfg);   /* blocks until engine is up and port is bound */

    chttpsvr_engine_wait();   /* block until SIGINT/SIGTERM triggers engine stop */
    chttpsvr_destroy(srv);    /* releases this server's engine reference */
    clog_close(logger);
    return 0;
}
```

### Construction and Lifecycle

`create_chttpsvr` / `create_chttpsvr_mp` always manage their own logger,
separate from any handle the caller passes in: passing `NULL` for `cl`
creates an internal logger that writes only FATAL messages to stderr;
passing a logger derives a new one from it (tagged `component=http-server`)
that the server owns from then on. The server's own logger is closed
automatically by `chttpsvr_destroy` / `__chttpsvr_destroy`; the caller's
original `cl` handle (when non-NULL) is never touched and remains the
caller's responsibility to close.

```c
clog logger = clog_open_fd(2, CLOG_INFO, NULL);
chttpsvr srv  = create_chttpsvr(logger, NULL);         /* default allocator; derives from logger */
chttpsvr srv  = create_chttpsvr_mp(mp, logger, &err);  /* custom allocator; derives from logger */
chttpsvr srv2 = create_chttpsvr(NULL, NULL);           /* internal stderr/FATAL-only logger */

chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT; /* Has the values below that can be modified as needed */
cfg.host                 = "0.0.0.0";            /* listen address; or "unix:///path/to/socket"
                                                    to bind a Unix domain socket instead (port
                                                    is then ignored); see Unix Domain Sockets below */
cfg.port                 = 8080;
cfg.max_body_size        = 4*1024*1024;          /* 4 MiB body limit (0 = unlimited);
                                                    exceeding it is a 413 (buffered) or a stream
                                                    error the handler observes via
                                                    chttpsvr_req_stream_error()
                                                    (streaming); either way the
                                                    connection closes afterward.
                                                    A buffered route whose
                                                    declared Content-Length
                                                    already exceeds this, or any
                                                    single chunk (chunked
                                                    Transfer-Encoding) whose
                                                    declared size does, is
                                                    rejected immediately, before
                                                    ever waiting for a body byte
                                                    that may never arrive */
cfg.read_timeout_ms      = 30000;                /* 30 s read timeout (baseline) */
cfg.idle_timeout_ms      = 60000;                /* 60 s keep-alive idle timeout */
cfg.stream_read_timeout_ms = 30000;              /* 30 s wait for the next body batch; 0 = unbounded */
cfg.max_body_read_duration_ms = 0;               /* 0 = no cap (default); caps the *total* time spent
                                                    reading one request's body, closing the loophole a
                                                    client that trickles bytes just fast enough to always
                                                    beat stream_read_timeout_ms would otherwise leave open */
cfg.response_write_timeout_ms = 0;               /* 0 = use stream_read_timeout_ms's value; bounds how
                                                    long a worker will wait per write(2)-equivalent call
                                                    while sending a response to a slow-reading client */
cfg.max_response_write_duration_ms = 0;          /* 0 = no cap (default); caps the *total* time spent
                                                    sending one response, closing the loophole a client
                                                    that reads just fast enough to always beat
                                                    response_write_timeout_ms would otherwise leave open */
cfg.max_header_bytes    = 0;                     /* 0 = library default (64 KiB); a request whose combined
                                                    request-line + header block exceeds this is rejected
                                                    (connection reset, no HTTP response: the header
                                                    block itself couldn't be parsed far enough to answer) */
cfg.max_connections     = 0;                     /* 0 = unlimited; once at capacity, new connections are
                                                    simply left pending in the kernel's own listen backlog
                                                    rather than accepted and immediately rejected; the
                                                    listener stops accepting until a slot frees, which can
                                                    take up to about a second to be noticed */
cfg.worker_thread_count  = 4;                    /* 0 = CPU core count */
cfg.worker_queue_capacity = 128;                 /* 0 = default (1024 * threads); CHTTPSVR_QUEUE_UNBOUNDED = no limit */
cfg.enable_keepalive     = false;                /* SO_KEEPALIVE on every accepted connection; no effect on
                                                    a Unix domain socket listener */
cfg.enable_reuseport     = false;                /* SO_REUSEPORT on the listening socket, letting multiple
                                                    chttpsvr instances (e.g. one per worker process) bind
                                                    the identical host:port for kernel-load-balanced accept;
                                                    no effect on a Unix domain socket listener */
cfg.ipv6_only            = false;                /* IPV6_V6ONLY on an AF_INET6 listener, so it does not also
                                                    implicitly accept IPv4-mapped connections; no effect on
                                                    an IPv4 or Unix domain socket listener */
cfg.tls                  = &tls_cfg;             /* optional TLS (chttp_tls_config_t) */

ccol_retval_t rv = chttpsvr_start(srv, &cfg);    /* starts the engine on first call */
/* Routes and middleware may be registered before or after chttpsvr_start. */

chttpsvr_stop(srv);     /* close this server's listener (other servers are unaffected) */
chttpsvr_destroy(srv);  /* drain in-flight requests; stop engine if last server */
```

Multiple servers can listen on different ports simultaneously:

```c
chttpsvr api  = create_chttpsvr(logger, NULL);
chttpsvr mgmt = create_chttpsvr(logger, NULL);

chttpsvr_config_t api_cfg  = CHTTPSVR_CONFIG_DEFAULT; api_cfg.port  = 8080;
chttpsvr_config_t mgmt_cfg = CHTTPSVR_CONFIG_DEFAULT; mgmt_cfg.port = 9090;

chttpsvr_start(api,  &api_cfg);   /* blocks until engine is up and port is bound */
chttpsvr_start(mgmt, &mgmt_cfg);

/* ... both servers serve concurrently ... */

chttpsvr_stop(api);
chttpsvr_stop(mgmt);

chttpsvr_destroy(api);
chttpsvr_destroy(mgmt);   /* engine stops after the last destroy */
```

### Unix Domain Sockets

Setting `host` to a `"unix://path"` string binds a Unix domain socket at that path instead of a TCP listener; `port` is then ignored (it may be left at 0):

```c
chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
cfg.host = "unix:///run/myapp.sock";
chttpsvr_start(srv, &cfg);
```

A stale socket file already present at that path is removed automatically before binding. A single `chttpsvr` instance listens on either TCP or a Unix socket, never both; an application wanting both creates two `chttpsvr` instances (they already share the same process-wide reactor at no extra cost). `chttpsvr_stop` unlinks the socket file it created.

The lifecycle macros follow the usual pattern:

```c
chttpsvr_construct(name, cl);         /* declare + init; fatal_err on failure */
chttpsvr_construct_scoped(name, cl);  /* same + auto-destroy on scope exit */
chttpsvr_declare(name);               /* declare without init */
chttpsvr_destroy(name);               /* destroy and set to CHTTPSVR_INVALID */
```

`chttpsvr` is an opaque value handle (a packed `{slot index, generation}` pair resolved through a library-owned slot table before the underlying server object is touched), not a pointer; never cast it to/from `void*` or compare it via a pointer cast; compare it directly against `CHTTPSVR_INVALID` (or use it in a truthiness check, since `CHTTPSVR_INVALID` is 0). Destroying the same handle twice, whether sequentially (a stale copy used after the first destroy already completed) or concurrently (two threads racing a destroy call on the same still-live handle), is a fatal error (`abort()`/`SIGABRT`), never a silent use-after-free or double-free.

### Routing

Register a handler for a method and URL pattern:

```c
chttpsvr_register_handler(srv, CHTTP_GET,    "/",              index_handler, NULL);
chttpsvr_register_handler(srv, CHTTP_POST,   "/users",         create_user,   NULL);
chttpsvr_register_handler(srv, CHTTP_GET,    "/users/{id}",    get_user,      NULL);
chttpsvr_register_handler(srv, CHTTP_DELETE, "/users/{id}",    delete_user,   NULL);
chttpsvr_register_handler(srv, CHTTP_PUT,    "/items/{id}/{sub}", update_item, NULL);
```

`{name}` segments are named path parameters. Multiple parameters per pattern are supported. Literal segments are URL-decoded before comparison using path-segment decoding rules (RFC 3986): percent-encoded sequences such as `%20` are decoded, but `+` is left as a literal `+` (not converted to a space; that is a query-string convention). Named-parameter values follow the same decoding, so a URL like `/users/hello+world` delivers `"hello+world"` to `chttpsvr_req_param`, not `"hello world"`.

**Parameter name restrictions:** the name inside `{...}` must consist entirely of characters from `[A-Za-z0-9_]`. Patterns with names containing any other character (spaces, hyphens, dots, etc.) are rejected at registration time with `ccol_invalid_args`.

Routes are matched in registration order across all registered routers. The server scans every route looking for a path-and-method match. If at least one route matches the path but none of those match the method, the server responds with 405 Method Not Allowed. If no route matches the path at all, it responds with 404. A request whose method the server does not recognize at all (a WebDAV verb, `TRACE`, `CONNECT`, a custom verb, ...) is rejected earlier still, with 501 Not Implemented, before route matching (or even path parsing) ever runs; see "Wildcard method (`CHTTP_ANY`)" below for why this matters even for a catch-all `CHTTP_ANY` registration. Path matching includes validation of percent-encoded sequences: a request with invalid encoding in any path segment (whether a literal segment (e.g. `/bad%ZZusers/{id}` against `/users/{id}`) or a captured `{name}` parameter (e.g. `/users/bad%ZZvalue` against `/users/{id}`)) does not match the route and returns 404 regardless of which methods are registered for that pattern. A percent-encoded sequence that decodes to a literal NUL byte (`%00`) is treated the same way, as invalid encoding, rather than being decoded and compared as-is; this keeps a segment containing an embedded NUL from being compared against a registered segment name with a C string function that would otherwise stop at the first NUL and ignore everything after it. This means multiple methods can be registered for the same path and all will work correctly regardless of registration order:

```c
chttpsvr_register_handler(srv, CHTTP_GET,  "/users",      list_users,   NULL);
chttpsvr_register_handler(srv, CHTTP_POST, "/users",      create_user,  NULL);
chttpsvr_register_handler(srv, CHTTP_GET,  "/users/{id}", get_user,     NULL);
chttpsvr_register_handler(srv, CHTTP_PUT,  "/users/{id}", update_user,  NULL);
```

**Trailing slashes:** A request path with a trailing slash does NOT match a pattern without one. For example, `GET /users/42/` returns 404 if only `/users/{id}` is registered. Register a separate pattern if you want to accept the trailing-slash form.

**Invalid patterns:** Route patterns must begin with `/`. Patterns that do not start with `/` (including the empty string) are rejected with `ccol_invalid_args`. Patterns containing consecutive slashes (e.g. `/foo//bar`) or a trailing slash (e.g. `/foo/`) are also rejected. Patterns whose `{name}` parameter segment contains characters outside `[A-Za-z0-9_]` are likewise rejected, as is a pattern that reuses the same `{name}` more than once (e.g. `/a/{id}/b/{id}`), since the second occurrence's captured value would otherwise be unreachable via `chttpsvr_req_param`. All of these cases would produce unreachable or misleading routes because incoming paths are never normalised; only an exact segment-by-segment match succeeds.

**Thread safety:** Route and middleware registration (`chttpsvr_register_handler`, `chttpsvr_use`, `chttpsvr_router_on`, `chttpsvr_router_use`, `chttpsvr_subrouter`) is thread-safe and may be called at any time; before or after `chttpsvr_start`, and concurrently with `chttpsvr_destroy` of the same server from another thread (a registration call racing a destroy simply returns `ccol_invalid_args`/NULL rather than touching freed memory). A reader-writer lock protects the routing tables so concurrent requests are never blocked by rare registration writes.

**Shutting a server down from within its own handler:** a request handler or middleware that wants to shut its own server down (e.g. an admin/shutdown endpoint) should call `chttpsvr_engine_stop()` and simply return, which is safe there by design. Calling `chttpsvr_destroy()` directly on the server currently running that handler is a fatal error instead (`abort()`/`SIGABRT`, the same class of misuse as a double-destroy): that worker thread's own in-flight request can never finish while it is itself blocked waiting to destroy the server it belongs to. Calling `chttpsvr_stop()` immediately followed by `chttpsvr_start()` to restart the server from within one of its own handlers hits the identical problem on the `chttpsvr_start()` half (`chttpsvr_stop()` alone is always safe there); `chttpsvr_start()` reports it gracefully with `ccol_not_permitted` instead of aborting, leaving the server in a safe, recoverable state that a later restart from a different thread can still complete normally. Calling `chttpsvr_engine_wait()` from within a handler or middleware running on any server's own worker pool is likewise a fatal error, for the same underlying reason: it would block the shared engine's own shutdown drain on this exact in-flight request finishing, which can never happen while it is the one blocked waiting.

The supported way to run more than one process serving traffic with `chttpserver` is one of the two patterns described under "Compile-Time Configuration" in section 4: `fork(2)` before any server in the process has been created and started, then have each resulting process (parent and child alike) call `create_chttpsvr_mp`/`chttpsvr_start` independently to build its own server from scratch; or `fork(2)` immediately followed by `exec(3)` (the pattern behind `popen`/`system`/launching a subprocess from within a request handler), which is fully supported regardless of whether any server is currently running, since the exec'd program replaces its process image entirely and never touches anything this library left behind.

As a defense-in-depth measure on top of that, a fresh `chttpsvr_start`/`_stop`/`_destroy`/`_register_handler`/`_register_streaming_handler`/`_use`/`_subrouter`/`chttpsvr_router_on`/`_on_stream`/`_use` call, from any thread, in either the parent or a freshly forked child not followed by `exec(3)`, never hangs waiting on a lock that some other (possibly no-longer-existing, since `fork()` duplicates only the calling thread) thread happened to hold at the instant of the fork; this also covers a fork landing mid-way through starting, stopping, or destroying that exact handle, or through a `chttpsvr_engine_stop()`-driven shutdown of the whole shared engine. Reviving a server that was already live and actively running work at the moment of such a fork is a different matter regardless of that measure, and is explicitly out of scope: `fork()` does not duplicate its reactor, worker-pool, or idle-timeout-sweep threads, so an inherited, already-started server can no longer accept new connections or process in-flight work in the child, and calling `chttpsvr_stop()` followed by `chttpsvr_start()` on that exact handle there does not bring it back either: the call reports `ccol_success`, but the server still cannot serve, since the engine reference it already held going into the fork is reused as-is rather than rebuilt. This mirrors how other HTTP servers treat the same situation: `net/http`'s own runtime documentation disclaims a bare, un-exec'd `fork()` for a multi-threaded/goroutine program entirely, and nginx/Apache's own prefork worker models only ever fork before any worker thread starts listening, never while one is already serving. This protection can be compiled out via `FORK_SAFETY_REQUIRED=0` for a caller that has no need for it.

`chttpsvr_register_handler` and `chttpsvr_router_on` return `ccol_invalid_args` if `srv` is `CHTTPSVR_INVALID` or a stale/already-destroyed handle, if `fn` is NULL, `pattern` is NULL, `pattern` does not start with `/`, `pattern` contains consecutive or trailing slashes, a `{name}` segment contains characters outside `[A-Za-z0-9_]`, or the same `{name}` is used more than once. `chttpsvr_register_streaming_handler` and `chttpsvr_router_on_stream` apply the same guards. `chttpsvr_use` and `chttpsvr_router_use` likewise return `ccol_invalid_args` for a NULL `fn`. `chttpsvr_subrouter` returns NULL if `srv` is `CHTTPSVR_INVALID` or a stale/already-destroyed handle, `prefix` is NULL, `prefix` does not start with `/`, or `prefix` contains consecutive slashes (e.g. `"//api"` or `"/a//b"`). `chttpsvr_router_on`, `chttpsvr_router_on_stream`, and `chttpsvr_router_use` also return `ccol_invalid_args` if the sub-router's owning server has since been destroyed.

**Wildcard method (`CHTTP_ANY`):** Pass `CHTTP_ANY` as the method to register a single handler that matches every HTTP method on the given pattern. Inside the handler, call `chttpsvr_req_method(req)` to determine which method was actually used. Because routing is first-wins, a method-specific route registered before a `CHTTP_ANY` route on the same pattern takes precedence for its method, while `CHTTP_ANY` catches every other method:

```c
/* GET uses get_user; all other methods (POST, PUT, DELETE, ...) use any_user. */
chttpsvr_register_handler(srv, CHTTP_GET,  "/users/{id}", get_user,  NULL);
chttpsvr_register_handler(srv, CHTTP_ANY,  "/users/{id}", any_user,  NULL);
```

`CHTTP_ANY` is a server-side routing sentinel only; do not pass it to the HTTP client API. It is a registration-time placeholder for "any of the seven concrete methods," never a real incoming request's method: a request whose method the server does not recognize at all (a WebDAV verb such as `PROPFIND`, `TRACE`, `CONNECT`, a custom verb, ...) never reaches any handler, including one registered with `CHTTP_ANY`. It is rejected with `501 Not Implemented` before routing (or even path/header parsing) ever runs, so `chttpsvr_req_method(req)` always returns one of the seven concrete methods, never a sentinel of any kind.

**Duplicate routes:** Registering the same method and pattern more than once is permitted and succeeds each time, but only the first registered handler is ever invoked (first-wins policy). There is no error or warning for duplicate registrations.

**Middleware limit:** Each router (the root router for global middleware, and each sub-router) caps its own middleware chain at 32 entries, enforced at registration time; `chttpsvr_use` / `chttpsvr_router_use` return `ccol_not_permitted` (without adding the entry) on the call that would exceed the cap for that router. Separately, dispatch time also enforces a combined cap of 32 for the effective chain of a given request (global middleware + the matched router's own middleware); since each side of that sum can independently reach 32, the combined count can still exceed 32 even when neither router hit its own registration-time cap, in which case the server responds with `500 Internal Server Error` on the affected request. In practice the limit is generous and is not expected to be reached.

**Root-router shadowing:** Routes registered directly on the server (via `chttpsvr_register_handler` / `chttpsvr_register_streaming_handler`) are part of the root router, which is always evaluated before any sub-router. A root-level route whose path AND method both match a request wins outright, shadowing a same-path sub-router route completely. A root-level route that matches the same path but a different method does not shadow the sub-router: every other root-level route is still tried for a same-path, same-method match first, and only once the whole root router has been exhausted does matching fall through to the sub-router, where a route whose own method matches the request is served normally (the same 405-vs-404 priority two same-router routes registered under different methods already follow). Avoid registering root-level routes whose paths overlap with a sub-router's prefix and pattern combination for the same method.

### Streaming Handlers

Routing happens as soon as headers are parsed, before any body byte is read; an unmatched route is rejected immediately without ever reading the body it's about to discard, and a matched route (buffered or streaming) is handed to the server's `ctpool` right away, regardless of body size. The reactor thread's job is therefore O(1) per request: it never blocks reading a large or slow body, nor does it ever block writing a rejection response (404/405/500 for an unmatched/malformed route, 501 for an unrecognized method, or 503 when `ctpool` is at capacity); every rejection's courtesy write runs on a small dedicated pool of its own, never the reactor thread, so a slow-reading client being told "no" cannot delay dispatch for any other, unrelated connection. The worker thread that picks up a matched request reads the body itself, batch by batch, directly off the socket via the same Content-Length/chunked framing logic the reactor would otherwise use; there is no temp file and no whole-body pre-buffering anywhere in the path.

**Pipelining:** a client that writes a second request before reading the first response (or simply writes several requests close enough together that the kernel coalesces them) is fully supported on a keep-alive connection, for both buffered and streaming routes: any bytes belonging to a further request that have already arrived in the same read as the current one's own headers or body are recognized as such and carried forward to the next request cycle rather than discarded, so no pipelined request is ever silently dropped regardless of how the bytes happened to be batched on the wire.

For a **buffered** handler, the worker reads the entire body into one growable buffer before invoking the handler, so `chttpsvr_req_body` still returns the complete body in one call. For a **streaming** handler, the worker hands each batch to `chttpsvr_req_read` as it arrives, so the handler can act on data before the rest of the body has even reached the server:

```c
chttpsvr_register_streaming_handler(srv, CHTTP_POST,
    "/upload", upload_handler, NULL);

static void upload_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    (void)ctx;
    char buf[4096];
    ssize_t n;
    while ((n = chttpsvr_req_read(req, buf, sizeof(buf))) > 0) {
        /* process chunk as it arrives; no need to wait for the rest */
    }
    if (n < 0) {
        /* connection dropped, timed out, or the body exceeded max_body_size;
           chttpsvr_req_stream_error(req) reports which. */
    }
    chttpsvr_resp_write_str(resp, "received");
}
```

`chttpsvr_req_read` blocks the calling worker thread (never the reactor) until at least one byte is available, the body ends, an error occurs, `chttpsvr_config_t.stream_read_timeout_ms` elapses with no new data, or (if set) `max_body_read_duration_ms` elapses. It returns `>0` bytes read, `0` at EOF or when `buflen` is 0 (a no-op, consistent with POSIX `read(2)` semantics), or `-1` on error; call `chttpsvr_req_stream_error(req)` immediately afterward to distinguish a timeout (`ccol_timed_out`), an oversized body (`ccol_msg_too_large`), or a dropped connection (`ccol_http_transfer_aborted`). Passing `NULL` for `buf` with `buflen == 0` is also valid and returns 0. Calling `chttpsvr_req_read` on a buffered (non-streaming) handler returns -1, and `chttpsvr_req_body` on a streaming handler returns `NULL`/0 (its body is never pre-extracted). A chunked body's trailer fields are indexed exactly like any other header and become retrievable through `chttpsvr_req_header` once they have actually been parsed: for a buffered handler this is transparent (the whole body, trailers included, is already read before the handler runs), while a streaming handler only sees a trailer field once its own `chttpsvr_req_read` calls have drained the body all the way to EOF (a return of 0); querying it any earlier returns `NULL`, indistinguishable from the field never having been sent.

If a request carries `Expect: 100-continue` and actually has a body (a `Content-Length` or chunked `Transfer-Encoding` was present), the first call to `chttpsvr_req_read` for that request sends the interim `100 Continue` response before attempting to read anything. A streaming handler that instead rejects a request outright (bad auth, unacceptable `Content-Type`, ...) by writing a final response without ever calling `chttpsvr_req_read` skips that interim response entirely, so the client sees the real rejection directly and is never asked to upload a body the server was not going to read. A buffered handler has no equivalent choice, since its whole body is always read before the handler ever runs; `chttpsvr` sends the interim response for a buffered route immediately, before that read begins. Either way, a request that carries `Expect: 100-continue` but has no body at all never receives the interim response: there is nothing to invite the client to upload.

`stream_read_timeout_ms` (default 30000ms) bounds how long a worker will wait for the *next* batch while reading a body, for both buffered and streaming routes; it exists because `read_timeout_ms`/`idle_timeout_ms` reset on any connection activity and so do not protect against a client that trickles bytes just fast enough to never trip them, tying up a worker thread indefinitely. Note that `stream_read_timeout_ms` itself resets on *any* new byte too, so a client that sends a byte or two just before each gap expires defeats it the same way; `max_body_read_duration_ms` (default 0, disabled) closes that loophole by capping the *total* time spent reading one request's body regardless of per-gap progress, independent of how many individual gaps it took to get there. The same asymmetry, and the same fix, exists on the response-send side: `response_write_timeout_ms` bounds only each individual `write(2)`-equivalent call, so a client that reads a byte or two just before every such call's own timeout expires can hold a worker thread for as long as it likes; `max_response_write_duration_ms` (default 0, disabled) caps the *total* time spent sending one response, closing that loophole the identical way. A courtesy rejection response (404/405/413/500/501/503, generated internally rather than by a handler) and the `Expect: 100-continue` interim `100 Continue` line are both additionally always bounded by a small internal ceiling regardless of `max_response_write_duration_ms`'s own value, including 0/disabled, since both are always a fixed, small shape with no legitimate reason to ever need longer; a smaller value configured there still applies in full. Configuring `stream_read_timeout_ms`, `max_body_read_duration_ms`, `response_write_timeout_ms`, and/or `max_response_write_duration_ms` to `0` ("wait indefinitely") does not turn a stalled connection into a permanent liability: shutting the server down (`chttpsvr_stop` immediately followed by `chttpsvr_start`, `chttpsvr_destroy`, or `chttpsvr_engine_stop`) still completes in bounded time by forcibly closing any connection whose worker thread is still blocked on it once that shutdown's own bounded, graceful wait is exhausted; a request that is still making progress is never affected by this.

The server's `ctpool` is created at `chttpsvr_start` time. Its capacity is controlled by `chttpsvr_config_t.worker_thread_count` and `worker_queue_capacity`. If the queue is full when a request arrives, the server responds immediately with `503 Service Unavailable`; it never stalls the reactor thread. `chttpsvr_stop` only closes the listener; connections it already accepted keep running and may still dispatch further requests through the same handle, including across a subsequent `chttpsvr_start` restart. That restart drains and replaces the old `ctpool` only once every already-in-flight request has completed (bounded, per the paragraph above, even if one of them is genuinely stalled), so it is always safe to restart a server this way even while such a connection is still active. `chttpsvr_stop` and a `chttpsvr_start` restart of the same handle are also safe to call concurrently from two different threads: the restart waits for an in-flight `chttpsvr_stop` call's own teardown of the old listener to fully finish before binding a new one on the same host/port, rather than racing a `bind()` against a socket the stop has not yet closed. `chttpsvr_start` is also safe to race against a concurrent, engine-wide teardown (`chttpsvr_engine_stop`, or the shared reactor's own graceful shutdown when the last other server referencing it is destroyed): it transparently retries until that teardown has finished, rather than racing it.

### Middleware

Middleware functions run before the route handler. They receive a `next` callback to continue the chain, or can short-circuit by not calling it:

```c
static void auth_mw(chttpsvr_req *req, chttpsvr_resp *resp,
                    void *ctx, chttpsvr_next_fn next) {
    const char *token = chttpsvr_req_header(req, "authorization");
    if (!token) {
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_UNAUTHORIZED);
        chttpsvr_resp_write_str(resp, "missing token");
        return;  /* do not call next; chain is terminated */
    }
    next(req, resp);  /* continue to the next middleware or handler */
}

/* Global middleware: runs for every request. */
chttpsvr_use(srv, auth_mw, NULL);
```

### Sub-Routers

Sub-routers group routes under a common path prefix and carry their own middleware chain:

```c
chttpsvr_router *api = chttpsvr_subrouter(srv, "/api/v1");
chttpsvr_router_use(api, rate_limit_mw, NULL);  /* runs for /api/v1/* only */
chttpsvr_router_on(api, CHTTP_GET, "/items",      list_items,  NULL);
chttpsvr_router_on(api, CHTTP_GET, "/items/{id}", get_item,    NULL);
chttpsvr_router_on(api, CHTTP_POST, "/items",     create_item, NULL);
```

The prefix must begin with `'/'`. A trailing slash is stripped automatically so `"/api/v1"` and `"/api/v1/"` are equivalent.

**The `"/"` prefix edge case:** After trailing-slash normalisation the prefix `"/"` is stored with `prefix_len = 1`. A `"/"` sub-router matches only the exact request path `"/"`; any other path, including one starting with a second slash (e.g. `"//foo"`), does **not** match and receives 404. If you need to catch all requests regardless of path, register routes directly on the server with `chttpsvr_register_handler` / `chttpsvr_register_streaming_handler` rather than using a sub-router with prefix `"/"`.

Global middleware (added via `chttpsvr_use`) runs before router middleware for all routes.

### Request API

```c
chttp_method_t  chttpsvr_req_method(req);
const char     *chttpsvr_req_path(req);
const char     *chttpsvr_req_raw_query(req);
const char     *chttpsvr_req_header(req, "content-type");
const void     *chttpsvr_req_body(req, &body_len);    /* buffered only */
ssize_t         chttpsvr_req_read(req, buf, buflen);  /* streaming only */
ccol_retval_t   chttpsvr_req_stream_error(req);       /* reason for the last -1 */
const char     *chttpsvr_req_param(req, "id");        /* named path param */

/* Multi-value query parameters (e.g. ?q=a&q=b): */
size_t n;
const char **vals = chttpsvr_req_query(req, "q", &n);
/* vals is NULL-terminated; the string values are valid for the handler
   lifetime, but the array pointer is a scratch buffer recycled on each call
   to chttpsvr_req_query; copy any pointers before the next call.
   NOTE: chttpsvr_req_query cannot distinguish OOM from key-absent; both
   return (NULL, count=0).  Call chttpsvr_req_query_oom(req) after a NULL
   return to tell them apart, or use chttpsvr_req_query_one for single-valued
   keys. */

/* Convenience for single-value query params (OOM-distinguishable): */
const char *val = NULL;
ccol_retval_t rv = chttpsvr_req_query_one(req, "sort", &val);
/* returns ccol_not_permitted if key appears more than once,
   ccol_not_enough_memory on OOM */
```

### Response API

```c
chttpsvr_resp_set_status(resp, CHTTP_STATUS_CREATED);
chttpsvr_resp_set_header(resp, "x-request-id", "abc123");
chttpsvr_resp_write(resp, data, len);        /* append raw bytes */
chttpsvr_resp_write_str(resp, "text");       /* append NUL-terminated string */
chttpsvr_resp_printf(resp, "id=%d name=%s", id, name); /* printf-style append */
chttpsvr_resp_write_json(resp, json, len);   /* sets Content-Type + appends */
```

The response is buffered and sent automatically when the handler returns. `chttpsvr_resp_write` uses an overflow-safe doubling strategy for buffer growth: it returns `ccol_not_enough_memory` when `len` would cause the total body length to overflow `size_t`, in addition to the normal allocator-failure case.

`chttpsvr_resp_set_header` rejects a value containing a CR or LF byte with `ccol_invalid_args`: name/value are written onto the wire with no further escaping, so a handler that reflects request-controlled data (a query parameter, a path parameter, an echoed request header) into a response header must not be able to inject arbitrary extra header lines or split the response in two by way of an unsanitized `\r`/`\n` in that data. `name` must be a non-empty RFC 7230 tchar-only token (letters, digits, and `!#$%&'*+-.^_`|~`); an empty name has no valid on-the-wire representation, and any other disallowed byte (a space or a literal `:`, say) is not itself a CRLF-injection vector but still produces a structurally malformed wire line a strict downstream parser could misread.

`chttpsvr_resp_set_header` accepts (and validates) a `"Connection"` or `"Content-Length"` header like any other name, but never sends either verbatim: the server always emits its own `Connection` header, reflecting whether the connection is actually kept open afterward, and always computes and emits its own `Content-Length` header from the response body actually written, since both decisions drive real wire framing and must never disagree with what the client is told. A `"Transfer-Encoding"` header name (case-insensitive) is rejected outright instead, with `ccol_invalid_args`: this server never transfer-codes a response body, so letting a handler-set Transfer-Encoding header reach the wire alongside the server's own auto-computed Content-Length header would produce an ambiguous framing an intermediary could misparse, mirroring `chttp_request_set_header`'s identical rejection on the client side.

**HEAD requests:** a handler reachable via `CHTTP_HEAD` (explicitly, or through a `CHTTP_ANY` registration) may write a response body exactly as it would for `GET`; per RFC 7231, the server reports the real body length via `Content-Length` (matching what a `GET` would have reported) but never writes the actual body bytes to the wire for a `HEAD` request.

**1xx/204/304 responses:** a handler may write a response body and then set (or have already set) an informational (1xx), `204 No Content`, or `304 Not Modified` status; per RFC 9110, none of the three may ever carry a body, regardless of method, so the body is never written to the wire for any of them, exactly like `HEAD`. A 1xx or 204 additionally never carries a `Content-Length` header at all, whether or not the handler wrote a body; `304` (like `HEAD`) still reports one, matching the length of whatever the handler wrote.

### Status Code Constants

Common status code constants from `chttp.h`:

| Constant | Value |
|---|---|
| `CHTTP_STATUS_OK` | 200 |
| `CHTTP_STATUS_CREATED` | 201 |
| `CHTTP_STATUS_NO_CONTENT` | 204 |
| `CHTTP_STATUS_BAD_REQUEST` | 400 |
| `CHTTP_STATUS_UNAUTHORIZED` | 401 |
| `CHTTP_STATUS_FORBIDDEN` | 403 |
| `CHTTP_STATUS_NOT_FOUND` | 404 |
| `CHTTP_STATUS_METHOD_NOT_ALLOWED` | 405 |
| `CHTTP_STATUS_INTERNAL_SERVER_ERROR` | 500 |
| `CHTTP_STATUS_SERVICE_UNAVAILABLE` | 503 |

### TLS

Pass a `chttp_tls_config_t` (from `chttp.h`) in the server config to enable TLS. The server uses OpenSSL via `ctls`, this library's own reactor-agnostic TLS wrapper:

```c
chttp_tls_config_t tls = {
    .cert_path       = "/etc/certs/server.crt",
    .key_path        = "/etc/certs/server.key",
    .ca_bundle_path  = NULL,   /* optional; enables mutual TLS when set */
};
cfg.tls = &tls;
```

`cert_path` and `key_path` are required together whenever `cfg.tls` is set: `chttpsvr_start` rejects any `cfg.tls` that does not have both non-NULL with `ccol_invalid_args`, rather than silently starting the server as plain, unencrypted HTTP. This covers a lone `cert_path`, a lone `key_path`, a `ca_bundle_path` set with the other two left NULL, and a `cfg.tls` left otherwise empty (for example `CHTTP_TLS_DEFAULT`, whose `cert_path`/`key_path` are both NULL, since that macro's verification-related fields are meant for the client side): a trust store, or a `chttp_tls_config_t` with no fields set at all, is never valid server-side configuration without a server identity certificate, unlike the client side (`chttpclient_set_tls`), where `ca_bundle_path` alone is the ordinary way to configure custom-CA verification with no client certificate involved. If the certificate/key pair, or `ca_bundle_path` when set, cannot actually be loaded (a missing/unreadable file or malformed contents), `chttpsvr_start` fails with `ccol_unexpected_failure` and no listener is registered; a `ca_bundle_path` that fails to load is never silently treated as "no mutual TLS configured", since a loaded CA bundle is what enables client-certificate verification on the resulting listener.

### API Reference

**Construction**

| Function | Description |
|---|---|
| `create_chttpsvr(cl, err)` | Create a server with the default allocator; `cl` may be NULL (an internal stderr/FATAL-only logger is used) or a parent logger to derive this server's logger from (tagged `component=http-server`); returns `CHTTPSVR_INVALID` on failure |
| `create_chttpsvr_mp(mp, cl, err)` | Create a server with a custom allocator; same `cl` semantics as `create_chttpsvr` |
| `__chttpsvr_destroy(srv)` | Destroy and free the server, including closing the server's own logger (`clog_close`); does not set the handle to `CHTTPSVR_INVALID`. Safe to call regardless of whether the shared engine is still running (releases this server's own reference, possibly triggering an asynchronous engine stop if it was the last one) or was already force-stopped via `chttpsvr_engine_stop()` while `srv` was still started (the server's own listener/connections/worker pool are quiesced exactly once either way). `srv` must be a currently-live handle: a stale handle (already destroyed, whether sequentially or concurrently), or a call made from within one of `srv`'s own request handlers/middleware, is a fatal error (`abort()`/`SIGABRT`), not a use-after-free/double-free; `CHTTPSVR_INVALID` itself remains a silent no-op |
| `chttpsvr_destroy(srv)` | Macro: calls `__chttpsvr_destroy` then sets the handle to `CHTTPSVR_INVALID` |

**Engine Lifecycle (shared, process-level, independent of `chttpclient`'s own engine)**

| Function | Description |
|---|---|
| `chttpsvr_set_engine_logger(cl)` | Derive an engine sub-logger from `cl` (adds `component=http-server-engine`) that receives the reactor's own diagnostics (TLS handshake failures, listen-socket bind failures, idle-timeout closures); may be called at any time, including after a full stop/restart cycle; returns `ccol_invalid_args` if `cl` is NULL |
| `chttpsvr_set_engine_mem_mgmt_procs(mp)` | Redirect the reactor's own internal memory management to `mp`, or to the default allocator if `mp` is NULL; must be called before the first `chttpsvr_start` (may be called again once the reactor has fully stopped); returns `ccol_invalid_args` if `mp` is non-NULL but has a NULL function pointer, or `ccol_not_permitted` if the reactor is already running |
| `chttpsvr_set_engine_num_reactor_threads(n)` | Pin the reactor to `n` OS threads, or restore the default (1) if `n` is 0; must be called before the first `chttpsvr_start` (may be called again once the reactor has fully stopped); returns `ccol_not_permitted` if the reactor is already running |
| `chttpsvr_engine_stop()` | Signal the shared reactor to stop; non-blocking and async-signal-safe; safe to call from a SIGINT/SIGTERM handler, including more than once (a repeated or overlapping call is a no-op). Has no effect on `chttpclient`'s own, independent engine |
| `chttpsvr_engine_wait()` | Block until the shared reactor has fully stopped; use as an escape hatch when you need a synchronous guarantee (e.g. after an external shutdown signal, or before reusing a just-freed port) |

**Per-Server Lifecycle**

| Function | Description |
|---|---|
| `chttpsvr_start(srv, cfg)` | Start the server; on the first call in the process, lazily starts the shared reactor and idle-sweep thread; returns `ccol_invalid_args` if `srv` is `CHTTPSVR_INVALID` or a stale/already-destroyed handle, or if `cfg->port` is 0 and `cfg->host` is not a `"unix://"` path; returns `ccol_not_permitted` if already started, or if called from within one of `srv`'s own request handlers/middleware to restart the very server running that handler |
| `chttpsvr_stop(srv)` | Close this server's listener; other servers continue running |

**Route Registration (root router)**

| Function | Description |
|---|---|
| `chttpsvr_register_handler(srv, method, pattern, fn, ctx)` | Register a buffered handler; runs on the server's ctpool |
| `chttpsvr_register_streaming_handler(srv, method, pattern, fn, ctx)` | Register a streaming handler; exposes `chttpsvr_req_read`; runs on the server's ctpool |
| `chttpsvr_use(srv, fn, ctx)` | Append global middleware |

**Sub-Routers**

| Function | Description |
|---|---|
| `chttpsvr_subrouter(srv, prefix)` | Create a sub-router under the given path prefix; prefix must start with '/' and must not contain consecutive slashes |
| `chttpsvr_router_on(router, method, pattern, fn, ctx)` | Register a buffered handler on the sub-router |
| `chttpsvr_router_on_stream(router, method, pattern, fn, ctx)` | Register a streaming handler on the sub-router |
| `chttpsvr_router_use(router, fn, ctx)` | Append middleware to the sub-router |

**Request**

| Function | Description |
|---|---|
| `chttpsvr_req_method(req)` | HTTP method of the request |
| `chttpsvr_req_path(req)` | URL path (decoded, without query string) |
| `chttpsvr_req_raw_query(req)` | Raw query string (without leading `?`); NULL if absent |
| `chttpsvr_req_header(req, name)` | Look up a request header (case-insensitive); NULL if absent |
| `chttpsvr_req_body(req, &len)` | Pointer to the buffered body bytes and length; NULL/0 on a streaming route (its body is never pre-extracted) |
| `chttpsvr_req_read(req, buf, n)` | Reads the next batch of body bytes for a streaming handler directly off the socket, blocking the worker (never the reactor) until data arrives, EOF, an error, `stream_read_timeout_ms` elapses, or (if set) `max_body_read_duration_ms` elapses; returns bytes read, 0 at EOF or when n==0 (no-op), -1 on error (NULL req, NULL buf with n>0, called from a buffered handler, timeout, oversized body, or dropped connection) |
| `chttpsvr_req_stream_error(req)` | Reports why the most recent `chttpsvr_req_read` returned -1: `ccol_timed_out`, `ccol_msg_too_large`, `ccol_http_transfer_aborted`, or `ccol_success`/`ccol_unexpected_failure` |
| `chttpsvr_req_param(req, name)` | Named path parameter (URL-decoded); NULL if not in pattern |
| `chttpsvr_req_query(req, key, &count)` | All values for a query parameter (lazy-parsed, NULL-terminated array); returns NULL on key-absent or OOM |
| `chttpsvr_req_query_one(req, key, &val)` | Single-value query parameter; `ccol_not_permitted` if key appears more than once; `ccol_not_enough_memory` on OOM |
| `chttpsvr_req_query_oom(req)` | Returns true if a previous `chttpsvr_req_query` call on this request failed due to OOM (distinguishes OOM from key-absent) |

**Response**

| Function | Description |
|---|---|
| `chttpsvr_resp_set_status(resp, code)` | Set the HTTP status code (default 200); values outside 100-999 are sent as 500 |
| `chttpsvr_resp_set_header(resp, name, value)` | Set or replace a response header |
| `chttpsvr_resp_write(resp, data, len)` | Append raw bytes to the response body |
| `chttpsvr_resp_write_str(resp, str)` | Append a NUL-terminated string |
| `chttpsvr_resp_printf(resp, format, ...)` | Format a printf-style string and append it to the response body |
| `chttpsvr_resp_write_json(resp, json, len)` | Append JSON body and set `Content-Type: application/json`; returns `ccol_invalid_args` when len is 0 |

---

## 20. Thread Safety

The library applies a consistent policy: **components that pass data between threads or provide shared services carry their own synchronisation; components used for single-threaded data manipulation are deliberately left unguarded.**

### Intentionally Unguarded Containers

`cvector`, `chashmap`, `cbstmap`, `cstring`, `cjson`, and `cyaml` contain no internal locks. This is a deliberate design decision, not an omission.

Per-operation locking provides a false sense of safety. Consider the check-then-act pattern that appears in virtually every real use of a map:

```c
/* Thread 1 */
if (chmap_get_ptr(map, key) == NULL) {
    chmap_insert(map, key, value);
}

/* Thread 2 - concurrent */
chmap_insert(map, key, other_value);
```

Even if each individual call were internally serialised, the window between `chmap_get_ptr` returning and `chmap_insert` executing is a race. Meaningful thread safety must be expressed at the level of the logical operation, not the individual call. Callers are expected to guard shared containers with the synchronisation primitives best suited to their access pattern.

`cjson` follows the same rule.  Concurrent calls to `cjson_parse_mp()` and `cjson_parse_n_mp()` on **independent** DOM trees are fully safe: the `err_str` out-parameter is caller-supplied and per-call; there is no shared state between concurrent parsers.  Access to any single DOM tree from multiple threads still requires external synchronisation.

`include/common.h` defines a set of thin, portable wrappers over pthreads (`mutex_t`, `cond_var_t`, `rw_lock_t`, `once_flag_t`, `thread_id_t`, `thread_ls_key_t`, their operation macros, and the thread creation/join, fork-handler, and thread-naming helpers built on `thread_id_t`). These exist purely as this library's own internal portability seam, so that a future port of the library to a pthread-less environment only requires retargeting `include/common.h`, not touching every module that needs synchronisation. They are not public API: callers guarding their own shared containers (per the previous section) should reach for whatever synchronisation primitive suits their application, such as raw pthreads or C11 `<threads.h>`, rather than these internal wrappers.

### Thread-Safe Components

The following components include their own internal synchronisation and are safe to call from multiple threads without external locking:

| Component | Synchronisation model |
|---|---|
| `mempool` | Internal mutex; disabled when created with `single_threaded = true` |
| `r_mempool` | Internal mutex; disabled when created with `single_threaded = true` |
| `circular_queue` | Internal mutex + condition variables |
| `dynamic_queue` | Internal mutex + condition variable |
| `channel` | Two internal circular queues (one per direction) |
| `clrucache` | Single mutex + per-entry condition variables; see constraints below |
| `clogger` | Mutex on the shared backing store; all handles writing to the same fd are fully serialised; see constraints below |
| `cthreadpool` | Internal mutex + condition variables; every public function, including `ctpool_shutdown_drain`, `ctpool_shutdown_immediate`, and `ctpool_destroy`, is safe to call concurrently with any other on the same handle; see constraints below |
| `chttpclient` | Internal pool mutex + condition variable; all public functions including `chttpclient_do`, `chttpclient_do_streaming`, `chttpclient_do_async`, `chttpclient_do_async_streaming`, `chttpclient_do_pooled`, and `chttpclient_do_pooled_streaming` are safe to call concurrently on the same handle |

### Per-Component Constraints

**`clrucache` eviction callback.** The callback passed to `clru_construct` is invoked **while the cache mutex is held**. It must not call back into the same cache handle, doing so will deadlock. It may allocate memory or write to a logger, but must not call `clru_get` or `clru_set` on the cache that triggered the eviction.

**`cthreadpool` shutdown and destroy.** `ctpool_shutdown_drain` and `ctpool_shutdown_immediate` are each idempotent: calling either one more than once, or calling both concurrently with each other on the same handle, is a safe no-op for whichever call arrives once shutdown has already started. `ctpool_destroy` may likewise be called concurrently with either shutdown function on the same still-live handle; it waits for that call to finish before releasing the pool's memory. The one hard restriction is `ctpool_destroy` itself: calling it a second time on a handle whose destroy has already completed, or racing it against a second, concurrent `ctpool_destroy` call on the very same still-live handle, is a fatal error rather than a safe no-op. All other public functions (`ctpool_submit`, `ctpool_try_submit`, `ctpool_timed_submit`, `ctpool_submit_future`, `ctpool_wait`, `ctpool_pending_count`, `ctpool_active_count`) are safe to call from multiple threads concurrently. The future functions (`ctpool_future_get`, `ctpool_future_done`, `ctpool_future_cancelled`, `ctpool_future_free`) are likewise safe to call concurrently on the same future object. `ctpool_future_create_detached` and `ctpool_future_fulfill` have no pool to serialise against at all; a detached future's only ordering requirement is that `ctpool_future_fulfill` is called exactly once.

**`cthreadpool` self-calls from within a task.** Calling `ctpool_destroy` on a pool from within a task (or that task's `on_complete` callback) currently executing on one of that pool's own worker threads is a fatal error, exactly like a stale handle. `ctpool_wait` called this way returns immediately instead of deadlocking the calling task against itself; `ctpool_shutdown_drain`/`ctpool_shutdown_immediate` called this way are a complete no-op, leaving the pool fully usable for a later, external shutdown call.

**`clogger` derived loggers.** `clog_derive` creates a sibling logger that shares the same fd, rotation state, and mutex as the root logger via the shared backing store. Writes from the root and all of its siblings are fully serialised with no additional locking required at the call site. The minimum-level check (`log_info`, `log_warn`, and similar macros) reads the per-logger level field without holding the mutex as a deliberate performance optimisation; a concurrent `clog_set_level` may therefore cause a single message near the boundary level to be inconsistently logged or dropped. This is intentional: the optimisation avoids mutex acquisition for every suppressed message, and the inconsistency window is not a data-corruption hazard.

---

## 21. Custom Memory Management

Every container accepts a `ccol_memmgmt_procs_t *` at creation time. Passing `NULL` selects the standard `malloc`/`calloc`/`realloc`/`free` family.

```c
typedef struct {
    ccol_malloc_t  malloc;
    ccol_free_t    free;
    ccol_calloc_t  calloc;
    ccol_realloc_t realloc;
} ccol_memmgmt_procs_t;
```

### Providing a Custom Allocator

```c
void *my_malloc(size_t size)           { return arena_alloc(&g_arena, size); }
void *my_calloc(size_t n, size_t size) { return arena_calloc(&g_arena, n, size); }
void *my_realloc(void *p, size_t size) { return arena_realloc(&g_arena, p, size); }
void  my_free(void *p)                 { arena_free(&g_arena, p); }

ccol_memmgmt_procs_t arena_mprocs = {
    .malloc  = my_malloc,
    .calloc  = my_calloc,
    .realloc = my_realloc,
    .free    = my_free,
};

cvec_construct_mp(vec, int, &arena_mprocs);
chmap_construct_mp(map, char*, double, &arena_mprocs);
```

### Driving a Container from a Ranged Pool

```c
r_mempool *node_pool = r_mempool_create(4, 10, 6,
                                        fallback_at_last_exhaustion,
                                        false, NULL, NULL);

void *pool_malloc(size_t size) {
    return r_mempool_alloc_entry(node_pool, size);
}

void *pool_calloc(size_t n, size_t size) {
    return r_mempool_calloc_entry(node_pool, n * size);
}

void *pool_realloc(void *p, size_t size) {
    return r_mempool_realloc_entry(node_pool, p, size);
}

void  pool_free(void *p) {
    r_mempool_free_entry(p);
}

ccol_memmgmt_procs_t pool_mprocs = {
    .malloc  = pool_malloc,
    .calloc  = pool_calloc,
    .realloc = pool_realloc,
    .free    = pool_free,
};

chmap_construct_mp(map, int, int, &pool_mprocs);

/* ... use map ... */

chmap_destroy(map);
r_mempool_destroy(node_pool);
```

---

## 22. License

MIT License

Copyright (c) 2026 - C Collections Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
