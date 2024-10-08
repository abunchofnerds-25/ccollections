# C Collections

A library of generic, type-safe data structures for C, built on C11 and GNU C extensions. It is designed for use in systems where correctness, performance, and predictable memory behaviour matter.

---

## Table of Contents

1. [Rationale](#1-rationale)
2. [Design Principles](#2-design-principles)
   - [Type Safety Without Code Generation](#21-type-safety-without-code-generation)
   - [Container Lifecycle Macros](#22-container-lifecycle-macros)
   - [Cross-Scope Type Recovery](#23-cross-scope-type-recovery)
   - [Error Handling](#24-error-handling)
3. [Building and Linking](#3-building-and-linking)
4. [Dynamic Array — `cvector`](#4-dynamic-array--cvector)
5. [Dynamic String — `cstring`](#5-dynamic-string--cstring)
6. [Sorting — `csort`](#6-sorting--csort)
7. [Hash Map — `chashmap`](#7-hash-map--chashmap)
8. [Ordered Map — `cbstmap`](#8-ordered-map--cbstmap)
9. [Memory Pools — `cmempool`](#9-memory-pools--cmempool)
10. [Thread Communication — `cthreadcomm`](#10-thread-communication--cthreadcomm)
11. [Thread Safety](#11-thread-safety)
12. [Custom Memory Management](#12-custom-memory-management)
13. [License](#13-license)

---

## 1. Rationale

Writing generic data structures in C is inherently difficult. The language provides no templates, no operator overloading, and no built-in reflection. The common responses to this problem—`void *` interfaces, preprocessor token-pasting, and external code-generation tools—each carry a significant cost: `void *` APIs discard type information at the call site and push the burden of correctness entirely onto the caller; token-pasting macros produce opaque, hard-to-debug expansions; code generators add build-system complexity and break the edit-compile-run cycle.

This library takes a different approach. It uses the C11 `_Generic` selection expression to perform type introspection directly at the call site, at compile time, without generating new code or introducing new tools into the build. The result is a set of containers whose public interfaces are type-aware, whose error handling is explicit, and whose memory behaviour is predictable and auditable.

The library is not a minimalist experiment. It covers the data structures needed in the majority of real systems work—dynamic arrays, hash maps, ordered maps, dynamic strings, memory pools, and thread communication primitives—each implemented with the same set of conventions so that learning one container transfers immediately to the next.

---

## 2. Design Principles

### 2.1 Type Safety Without Code Generation

Every container macro inspects its argument with `_Generic` at the call site and records a `ccol_data_type` enum in the container's header struct. This enum drives all subsequent type-dependent decisions at runtime:

- `chashmap` selects open-addressing or separate-chaining based on key and value types.
- `cbstmap` selects signed, unsigned, or lexicographic key comparison.
- `csort` selects the default comparator.
- Internal serialisation into `cmap_pair` chooses the correct path.

The key macros are defined in `include/common.h`: `is_integral_type()`, `is_char_ptr()`, `is_char_array()`, and `determine_ccol_data_type()`.

### 2.2 Container Lifecycle Macros

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

### 2.3 Cross-Scope Type Recovery

The type-dispatching macros rely on a hidden companion variable created by `*_declare` or `*_construct`. When a container is passed across a function boundary, this variable is not present in the new scope. The `*_redeclare` macro re-establishes it, allowing all type-safe macros to function correctly:

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

### 2.4 Error Handling

Functions return `ccol_retval_t`, an enum whose value zero indicates success and whose negative values indicate specific failure conditions:

```c
typedef enum {
    ccol_unexpected_failure = -10,
    ccol_msg_too_large      = -9,
    ccol_container_empty    = -8,
    ccol_container_full     = -7,
    ccol_timed_out          = -6,
    ccol_not_permitted      = -5,
    ccol_invalid_args       = -4,
    ccol_key_not_found      = -3,
    ccol_key_already_present= -2,
    ccol_not_enough_memory  = -1,
    ccol_success            =  0
} ccol_retval_t;
```

The convenience macros call `fatal_err()` on unrecoverable failures—caller bugs and resource exhaustion—causing immediate termination with a diagnostic message. When finer control is required, the underlying functions can be called directly and their return values inspected.

---

## 3. Building and Linking

Clone the repository and run `make` to produce `libccollections.so` and a demonstration binary:

```bash
make              # Build the shared library and the demo binary
make run          # Run the demo (sets LD_LIBRARY_PATH=. automatically)
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
```

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
```

The compiler must support C11 and GNU extensions (`-std=gnu11`). The library compiles cleanly under both GCC and Clang; diagnostic pragma guards for each compiler are present in the headers.

---

## 4. Dynamic Array — `cvector`

`cvector` is a heap-allocated, automatically resizing array. It provides amortised O(1) insertion at the end, O(1) indexed access, and stable O(n log n) sorting. A vector maintains a minimum capacity of four elements, doubles its allocation when full, and halves it when occupancy drops below one quarter.

**Header:** `#include <cvector.h>`

### Basic Usage

```c
/* Construct a vector of integers */
cvec_construct(scores, int);

/* Push values */
cvec_push_rvalue(scores, 95);
cvec_push_rvalue(scores, 82);
cvec_push_rvalue(scores, 78);

/* Index-based access — cvec_at returns a modifiable lvalue */
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

The distinction between `cvec_push` and `cvec_push_rvalue` exists because the type-dispatching macro internally takes the address of its argument. Addressable variables use `cvec_push`; literals and expressions use `cvec_push_rvalue`:

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

### Reference: Core Operations

| Macro / Function | Description |
|---|---|
| `cvec_construct(v, T)` | Declare and initialise |
| `cvec_construct_scoped(v, T)` | Declare, initialise, and register auto-cleanup |
| `cvec_construct_mp(v, T, mprocs)` | Declare and initialise with custom allocator |
| `cvec_push(v, var)` | Append an lvalue |
| `cvec_push_rvalue(v, expr)` | Append an rvalue or expression |
| `cvec_pop(v)` | Remove and return the last element |
| `cvec_at(v, i)` | Access element at index `i` (modifiable lvalue) |
| `cvec_size(v)` | Number of elements currently stored |
| `cvec_sort(v)` | Sort in place using the default comparator |
| `cvector_sort_with_comparison_proc(v, cmp)` | Sort with a custom comparator |
| `cvec_reset(v)` | Remove all elements and reset capacity |
| `cvec_destroy(v)` | Destroy and set pointer to `NULL` |

---

## 5. Dynamic String — `cstring`

`cstring` is a heap-allocated string with automatic capacity management. Its internal buffer always holds a null-terminated C string, making it directly compatible with standard library functions. Capacity grows to the next power of two on demand, with a minimum of 16 bytes.

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

/* Derived strings — caller is responsible for destroying these */
cstr sub  = cstr_substring(s, 1, 4);
cstr copy = cstr_copy(s, NULL);
cstr_destroy(sub);
cstr_destroy(copy);

cstr_destroy(s);
```

> **Pointer stability:** `cstr_c_str()` returns a pointer into the string's internal buffer. Any mutating operation—`cstr_append`, `cstr_insert`, `cstr_replace`, and others—may reallocate the buffer, invalidating all previously obtained raw pointers.

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

---

## 6. Sorting — `csort`

`csort` provides a stable, iterative bottom-up mergesort. It operates on any collection type through a getter abstraction, and provides default comparators for all standard C arithmetic types selected via `_Generic`. The integration with `cvector` is the most common usage path.

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

**Complexity guarantees:** O(n log n) in all cases; O(n) auxiliary space; stable (equal elements preserve their original order); iterative (no recursion, no stack overflow risk for large inputs).

---

## 7. Hash Map — `chashmap`

`chashmap` is an associative container with O(1) average-case insertion, lookup, and deletion. A distinctive feature is that it selects one of two internal implementations at compile time, based on the types of the key and value.

**Header:** `#include <chashmap.h>`

### Implementation Selection

**Open-addressing** is selected when both the key and the value are integral types no wider than eight bytes. It uses compact 17-byte slots (8-byte key, 8-byte value, 1-byte metadata), Fibonacci hashing for integers, and linear probing. Load factor thresholds are 0.70 (grow) and 0.25 (shrink), with a 2× scale factor. There are zero per-entry heap allocations, and cache locality is quite good.

**Separate chaining** is selected for all other type combinations. It uses a linked-list per bucket, XXHash64 for content-based hashing, Small String Optimisation (23-byte inline buffer for short strings), and a doubly-linked list that preserves insertion order. The minimum bucket count is 64 (always a power of two), and the scale factor is 4×.

The selection happens transparently; the same macro interface is used in both cases.

### Basic Usage

```c
/* Both key and value are integral — open-addressing is selected */
chmap_construct(counters, int, long);

/* String key — separate chaining is selected */
chmap_construct(index, char*, int);

/* Insertion: keys and values must be lvalues (see note below) */
int id = 42;
long count = 1000;
chmap_insert(counters, id, count);

char *word = "hello";
int freq = 5;
chmap_insert(index, word, freq);

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

The `chmap_for_each` macro is the recommended iteration pattern. It declares the iterator variable in its own scope, avoiding name collisions:

```c
chmap_for_each(index, it, {
    printf("%s: %d\n", *chmap_iter_key_ptr(it), *chmap_iter_val_ptr(it));
});
```

When direct control over iteration is required, the iterator can be managed manually. Note that the iterator variable must be declared outside the loop:

```c
chmap_iter_declare(index, it);
for (it = chmap_begin(index); it != NULL; it = chmap_iter_next(it)) {
    printf("%s -> %d\n", *chmap_iter_key_ptr(it), *chmap_iter_val_ptr(it));
}
```

Iteration order is unspecified for both implementations.

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

chmap_for_each(freq, it, {
    printf("%-8s %d\n", *chmap_iter_key_ptr(it), *chmap_iter_val_ptr(it));
});

chmap_destroy(freq);
```

### Memory Ownership for String Keys and Values

When the key or value type is `char *`, the map copies the string content into its own storage (inline if it fits in 23 bytes, heap-allocated otherwise). The original pointer may be freed immediately after insertion without affecting the map. Retrieved string pointers point into the map's internal storage and must not be freed by the caller.

For non-string pointer values—such as `Point *`—the map stores the pointer itself, not a copy of the pointed-to object. Lifetime management of the pointed-to data is the caller's responsibility:

```c
typedef struct { int x, y; } Point;
chmap_construct(coords, int, Point*);

Point *p = malloc(sizeof(Point));
p->x = 10; p->y = 20;
int key = 1;
chmap_insert(coords, key, p);

/* Before destroying the map, free all pointed-to objects */
chmap_for_each(coords, it, {
    free(*chmap_iter_val_ptr(it));
});
chmap_destroy(coords);
```

---

## 8. Ordered Map — `cbstmap`

`cbstmap` is an associative container implemented as a fully iterative (non-recursive) AVL tree. It maintains keys in sorted order and provides O(log n) insertion, deletion, and lookup. In-order iteration visits entries from smallest to largest key.

**Header:** `#include <cbstmap.h>`

Automatic key comparison is provided for signed integer keys, unsigned integer keys, and `char *` keys (using `strcmp`). For other key types, a custom comparison function must be supplied.

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
cbmap_for_each(scores, it, {
    printf("%3d  %s\n", *cbmap_iter_key_ptr(it), *cbmap_iter_val_ptr(it));
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

---

## 9. Memory Pools — `cmempool`

The library provides two pool allocators: a fixed-size pool (`mempool`) and a ranged pool (`r_mempool`). Both offer O(1) allocation and deallocation, optional thread safety, and an optional fallback to the system allocator when the pool is exhausted.

**Header:** `#include <cmempool.h>`

### Fixed-Size Pool — `mempool`

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

### Ranged Pool — `r_mempool`

A `r_mempool` covers allocation requests across a configurable range of power-of-two sizes. It maintains an internal sub-pool for each size class and selects the smallest fitting class for each request. Requests that exceed the largest class can fall back to the system allocator.

The following table illustrates the structure produced by `r_mempool_create(4, 12, 9, ...)`:

| Element Size | Element Count |
|---|---|
| 2⁴  = 16 bytes   | 2⁹ = 512 |
| 2⁵  = 32 bytes   | 2⁸ = 256 |
| 2⁶  = 64 bytes   | 2⁷ = 128 |
| 2⁷  = 128 bytes  | 2⁶ = 64  |
| 2⁸  = 256 bytes  | 2⁵ = 32  |
| 2⁹  = 512 bytes  | 2⁴ = 16  |
| 2¹⁰ = 1024 bytes | 2³ = 8   |
| 2¹¹ = 2048 bytes | 2² = 4   |
| 2¹² = 4096 bytes | 2¹ = 2   |

```c
r_mempool *rpool = r_mempool_create(
    4, 12, 9,
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

### Driving Other Containers from a Pool

Any container that accepts a `ccol_memmgmt_procs_t *` can be directed to allocate from a pool. See [Section 12](#12-custom-memory-management) for the complete pattern.

---

## 10. Thread Communication — `cthreadcomm`

The thread communication module provides three primitives for safe message passing between threads: a bounded circular queue, an unbounded dynamic queue, and a bidirectional channel. All three use a zero-copy ownership transfer model: the sender's pointer is set to `NULL` on a successful send, and the receiver becomes the sole owner of the data.

**Header:** `#include <cthreadcomm.h>`

### The Message Type

```c
typedef struct {
    void   *data;  /* Pointer to heap-allocated payload; NULL is a valid sentinel */
    size_t  size;  /* Size of the payload in bytes */
} c_message_t;
```

### Circular Queue — Bounded, Blocking

`circular_queue` holds a fixed number of messages. A sender blocks when the queue is full; a receiver blocks when it is empty. This backpressure mechanism is the primary tool for rate-limiting producers.

```c
circular_queue *cq = circular_queue_create(16, NULL);

/* Producer */
c_message_t msg = { .data = strdup("task payload"), .size = 13 };
circq_send_zc(cq, &msg);
/* msg.data is now NULL — ownership has been transferred */

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

### Dynamic Queue — Unbounded

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

### Channel — Bidirectional, Owner–Worker Pattern

A `channel` wraps two circular queues—one in each direction—and routes messages automatically based on the identity of the calling thread. The thread that calls `channel_create` is the owner; all other threads are workers. This removes the need for separate queue handles at the cost of a thread-identity check on each operation.

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

### Example: Producer–Consumer with Sentinel Termination

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

### Multiplexed Waiting — `ccol_select`

`ccol_select` blocks until any one of a set of queue or file descriptor sources becomes ready, analogous to POSIX `select(2)` or `poll(2)` but integrated with the queue primitives. `ccol_select_timed` adds a millisecond deadline measured on `CLOCK_MONOTONIC`.

```c
circular_queue *q0 = circular_queue_create(8, NULL);
dynamic_queue  *dq  = dynamic_queue_create(NULL);
int pfd[2];
pipe(pfd);

c_message_t msg;
size_t ready_index;

ccol_retval_t rc = ccol_select_va(&msg, &ready_index,
    selectable_from_circq(q0,    ccol_select_read),
    selectable_from_dynq(dq,     ccol_select_read),
    selectable_from_fd(pfd[0],   ccol_select_read));

if (rc == ccol_success) {
    if (ready_index == 2) {
        /* A file descriptor fired; msg.data contains the read data (caller must free).
           msg.data is NULL on EOF. */
    } else {
        /* A queue fired; caller owns msg.data */
    }
    free(msg.data);
}
```

The timed variant returns `ccol_timed_out` if the deadline expires before any source fires. A timeout of `0` polls without blocking; `-1` blocks indefinitely (equivalent to `ccol_select`):

```c
ccol_retval_t rc = ccol_select_timed_va(&msg, &ready_index, /*timeout_ms=*/200,
    selectable_from_circq(q0, ccol_select_read),
    selectable_from_fd(pfd[0], ccol_select_read));

if (rc == ccol_timed_out) {
    /* No source was ready within 200 ms */
}
```

To protect against a misbehaving peer sending unbounded data on a file descriptor, use `selectable_from_fd_limited`. If the incoming data exceeds the cap, `ccol_select` returns `ccol_msg_too_large` and discards the partial buffer:

```c
ccol_retval_t rc = ccol_select_va(&msg, &ready_index,
    selectable_from_fd_limited(pfd[0], ccol_select_read, /*max_bytes=*/65536));
```

**Key properties:**

- A queue win is zero-copy: the message is dequeued atomically and ownership transferred.
- A file descriptor read win: `msg.data` is heap-allocated by `ccol_select` (caller must `free`). `msg.size` is the byte count. EOF yields `msg.data = NULL`.
- A file descriptor write win: readiness is reported only; the caller then calls `write(2)`.
- Queue-only selectable sets use a condition variable path with no `epoll` overhead. Any file descriptor in the set switches the implementation to `epoll(7)` automatically.

---

## 11. Thread Safety

### Intentionally Unguarded Containers

The vector, hash map, BST map, and dynamic string contain no internal locks. This is a deliberate design decision, not an omission.

Per-operation locking provides a false sense of safety. Consider the check-then-act pattern that appears in virtually every real use of a map:

```c
/* Thread 1 */
if (chmap_get_ptr(map, key) == NULL) {
    chmap_insert(map, key, value);
}

/* Thread 2 — concurrent */
chmap_insert(map, key, other_value);
```

Even if each individual call were internally serialised, the window between `chmap_get_ptr` returning and `chmap_insert` executing is a race. Meaningful thread safety must be expressed at the level of the logical operation, not the individual call. Callers are expected to guard shared containers with the synchronisation primitives best suited to their access pattern.

The library provides thin, portable wrappers over pthreads in `include/common.h`:

```c
/* Exclusive mutex */
mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
mutex_lock(lock);
chmap_insert(map, key, value);
mutex_unlock(lock);

/* Reader–writer lock for read-heavy workloads */
rw_lock_t rw = PTHREAD_RWLOCK_INITIALIZER;

rw_lock_rdlock(rw);
int val = chmap_get(map, key);
rw_lock_unlock(rw);

rw_lock_wrlock(rw);
chmap_insert(map, key, new_value);
rw_lock_unlock(rw);
```

### Thread-Safe Components

The following components include their own synchronisation and are safe to use from multiple threads without external locking:

| Component | Thread Safety |
|---|---|
| `mempool` | Thread-safe unless created with `single_threaded = true` |
| `r_mempool` | Thread-safe unless created with `single_threaded = true` |
| `circular_queue` | Always thread-safe |
| `dynamic_queue` | Always thread-safe |
| `channel` | Always thread-safe |

---

## 12. Custom Memory Management

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
void *my_malloc(size_t size)              { return arena_alloc(&g_arena, size); }
void *my_calloc(size_t n, size_t size)    { return arena_calloc(&g_arena, n, size); }
void *my_realloc(void *p, size_t size)    { return arena_realloc(&g_arena, p, size); }
void  my_free(void *p)                    { arena_free(&g_arena, p); }

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

void *pool_malloc(size_t size)              { return r_mempool_alloc_entry(node_pool, size); }
void *pool_calloc(size_t n, size_t size)    { return r_mempool_calloc_entry(node_pool, n * size); }
void *pool_realloc(void *p, size_t size)    { return r_mempool_realloc_entry(node_pool, p, size); }
void  pool_free(void *p)                    { r_mempool_free_entry(p); }

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

## 13. License

MIT License

Copyright (c) 2026 — C Collections Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
