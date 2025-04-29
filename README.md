# C Collections

`c_collections` is a production-quality library of generic, type-safe data structures for C. It covers the full breadth of what most systems software needs: dynamic arrays, hash maps, ordered maps, dynamic strings, memory pools, a structured logger, a JSON parser, and inter-thread communication primitives - all under a single, consistent API.

Type safety is enforced at compile time using C11 `_Generic` selection, with no code-generation tools or external build-system steps required. Every module follows the same lifecycle conventions (`*_construct`, `*_destroy`, and `*_scoped` variants for automatic cleanup), so learning one container transfers immediately to the next.

The library targets contexts where correctness, performance, and predictable memory behaviour matter. It compiles clean under both GCC and Clang at `-Wall -Wextra -Werror`, and every module ships with a comprehensive test suite validated under Valgrind.

---

## Table of Contents

1. [Rationale](#1-rationale)
2. [Alternatives Considered](#2-alternatives-considered)
3. [Design Principles](#3-design-principles)
   - [Type Safety Without Code Generation](#31-type-safety-without-code-generation)
   - [Container Lifecycle Macros](#32-container-lifecycle-macros)
   - [Cross-Scope Type Recovery](#33-cross-scope-type-recovery)
   - [Error Handling](#34-error-handling)
4. [Building and Linking](#4-building-and-linking)
5. [Dynamic Array - `cvector`](#5-dynamic-array--cvector)
6. [Dynamic String - `cstring`](#6-dynamic-string--cstring)
7. [Sorting - `csort`](#7-sorting--csort)
8. [Hash Map - `chashmap`](#8-hash-map--chashmap)
9. [Ordered Map - `cbstmap`](#9-ordered-map--cbstmap)
10. [Unified Iteration - `citerators`](#10-unified-iteration--citerators)
11. [Memory Pools - `cmempool`](#11-memory-pools--cmempool)
12. [Thread Communication - `cthreadcomm`](#12-thread-communication--cthreadcomm)
13. [LRU Cache - `clrucache`](#13-lru-cache--clrucache)
14. [Structured Logger - `clogger`](#14-structured-logger--clogger)
15. [JSON Parser / Serializer / DOM - `cjson`](#15-json-parser--serializer--dom--cjson)
16. [YAML Parser / Serializer / DOM - `cyaml`](#16-yaml-parser--serializer--dom--cyaml)
17. [Thread Safety](#17-thread-safety)
18. [Custom Memory Management](#18-custom-memory-management)
19. [License](#19-license)

---

## 1. Rationale

Writing generic data structures in C is inherently difficult. The language provides no templates, no operator overloading, and no built-in reflection. Each of the common responses to this problem (`void *` interfaces, preprocessor token-pasting, and external code-generation tools) carry a significant cost: `void *` APIs discard type information at the call site and push the burden of correctness entirely onto the caller; token-pasting macros produce opaque, hard-to-debug expansions; code generators add build-system complexity and break the edit-compile-run cycle.

This library takes a different approach. It uses the C11 `_Generic` selection expression to perform type introspection directly at the call site, at compile time, without generating new code or introducing new tools into the build. The result is a set of containers whose public interfaces are type-aware, whose error handling is explicit, and whose memory behaviour is predictable and auditable.

The library is not a minimalist experiment. It covers the data structures needed in the majority of real systems work (dynamic arrays, hash maps, ordered maps, dynamic strings, memory pools, and thread communication primitives) each implemented with the same set of conventions so that learning one container transfers immediately to the next.

---

## 2. Alternatives Considered

Several well-established libraries provide generic data structures for C programs. Understanding the motivation for this library requires understanding what each alternative offers and where its design constraints create friction in certain contexts.

**GLib.** The GNOME utility library provides a comprehensive set of containers (`GHashTable`, `GArray`, `GPtrArray`, `GList`, `GTree`) and is mature, extensively tested, and widely deployed. Its primary trade-off is that all interfaces accept `gpointer` (a typedef for `void *`), so type information is absent at the call site. Correct usage requires explicit casts, and type errors manifest at runtime rather than at compile time. GLib is also a substantial dependency: importing it for its data structures alone introduces a large runtime with its own threading model, type system, and object hierarchy. For projects already built on GTK or GNOME infrastructure this cost is already paid, but for a self-contained systems library it represents significant overhead.

**uthash.** Troy Hanson's single-header hash table is zero-dependency and widely used in embedded and systems code. Its design is intrusive: a hash handle is embedded directly in the user's struct, and the map is accessed via a pointer to that struct. This eliminates separate allocation for key/value pairs and gives very low overhead, but it constrains the data model; a struct can participate in only one uthash table unless multiple handles are embedded manually. While string keys are supported natively, other key types require additional macro boilerplate. uthash also covers only the hash table use case; it does not address ordered maps, dynamic strings, memory pools, or inter-thread communication.

**klib.** Heng Li's klib takes a philosophy similar in spirit to this library: generic containers implemented entirely in C headers using macros. `kvec` and `khash` are efficient and appear in performance-sensitive open-source code. The key distinction is that klib uses preprocessor token-pasting to generate a new family of typed functions for each instantiation (`KHASH_MAP_INIT_INT`, `KHASH_MAP_INIT_STR`, and similar). Adding a new key/value type combination requires an explicit instantiation declaration; there is no mechanism to infer or dispatch on type automatically at the call site. The library covers hash maps and dynamic arrays but does not provide ordered maps, dynamic strings, memory pools, or threading primitives.

**stb_ds.** Sean Barrett's `stb_ds.h` provides hash maps and dynamic arrays in a single-header, zero-dependency style valued for its simplicity and portability. Internally, values are accessed through typed pointer casts over `void *` storage, and type consistency is the caller's responsibility. The library does not cover ordered maps, dynamic strings, memory pools, or inter-thread messaging, and it provides no mechanism for automatic cleanup or custom allocator injection.

**Where this library differs.** The design goal was a library that satisfies four requirements simultaneously: compile-time type awareness at the call site without code generation or external tools; a uniform macro API across all container kinds so that learning one container transfers immediately to the next; structural integration between components; any container can be backed by a memory pool using a common allocator interface; and thread communication primitives that follow the same ownership and lifecycle model as the rest of the library. No single library in common use addresses all four of these requirements together. The trade-off is a dependency on a C11-capable compiler with GNU extensions, and `_Generic` expressions that produce verbose error messages when an unsupported type is supplied which are acceptable constraints in the contexts for which this library was designed.

---

## 3. Design Principles

### 3.1 Type Safety Without Code Generation

Every container macro inspects its argument with `_Generic` at the call site and records a `ccol_data_type` enum in the container's header struct. This enum drives all subsequent type-dependent decisions at runtime:

- `chashmap` selects open-addressing or separate-chaining based on key and value types.
- `cbstmap` selects signed, unsigned, or lexicographic key comparison.
- `csort` selects the default comparator.
- Internal serialisation into `cmap_pair` chooses the correct path.

The key macros are defined in `include/common.h`: `is_integral_type()`, `is_char_ptr()`, `is_char_array()`, and `determine_ccol_data_type()`.

### 3.2 Container Lifecycle Macros

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

### 3.4 Error Handling

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

The convenience macros call `fatal_err()` on unrecoverable failures (caller bugs and resource exhaustion) causing immediate termination with a diagnostic message. When finer control is required, the underlying functions can be called directly and their return values inspected.

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
cd tests/clogger   && make test
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
#include <clrucache.h>
#include <clogger.h>
#include <cjson.h>
#include <cyaml.h>
```

`cvector.h`, `chashmap.h`, and `cbstmap.h` each automatically include `citerators.h`, so the unified iteration API (`ccol_begin`, `ccol_for_each`, `ccol_iter_declare`, and related macros) is available whenever any one of those container headers is included.

### Quick Start

The following example demonstrates three modules working together: an incoming JSON payload is parsed, a nested field is updated, and the result is logged with structured context attached to every line.

```c
#include <cjson.h>
#include <clogger.h>

int handle_webhook(clog lg, const char *payload) {
    char *err = NULL;
    cjson doc = cjson_parse(payload, &err);
    if (!doc) {
        log_error(lg, "JSON parse failed: %s", err);
        free(err);
        return -1;
    }

    /* Navigate to a nested field and update it in place */
    cjson status = cjson_get(doc, "event.status");
    if (status && cjson_type(status) == CJSON_STRING)
        cjson_set(doc, "event.status", "processed");

    char *out = cjson_serialize(doc);
    log_info(lg, "forwarding: %s", out);
    cjson_serialize_free(out);

    cjson_destroy(doc);
    return 0;
}

int main(void) {
    clog lg = clog_open_fd_mp(2, CLOG_INFO, NULL);
    clog_set_field(lg, "service", "webhooks");
    clog_set_field(lg, "env",     "prod");

    handle_webhook(lg, "{\"event\":{\"type\":\"push\",\"status\":\"pending\"}}");

    clog_close(lg);
    return 0;
}
```

Each module is fully independent: include only the headers your translation unit needs.

The compiler must support C11 and GNU extensions (`-std=gnu11`). The library compiles cleanly under both GCC and Clang; diagnostic pragma guards for each compiler are present in the headers.

---

## 5. Dynamic Array - `cvector`

`cvector` is a heap-allocated, automatically resizing array. It provides amortised O(1) insertion at the end, O(1) indexed access, and stable O(n log n) sorting. A vector maintains a minimum capacity of four elements, doubles its allocation when full, and halves it when occupancy drops below one quarter.

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

### Real-World Use Case: Paginated Query Results

A common pattern in API servers is to accumulate rows from a database cursor into a vector, sort them by a field, and return a page window. The vector handles growth automatically, and the scoped variant ensures cleanup even on early return:

```c
int cmp_score_desc(const void *a, const void *b) {
    const Row *ra = (const Row *)a;
    const Row *rb = (const Row *)b;
    return (rb->score > ra->score) - (rb->score < ra->score);
}

void render_page(DbCursor *cursor, JsonResponse *resp,
                 size_t page, size_t page_size) {
    cvec_construct_scoped(rows, Row);

    Row row;
    while (db_cursor_next(cursor, &row) == DB_OK)
        cvec_push(rows, row);

    cvector_sort_with_comparison_proc(rows, cmp_score_desc);

    size_t start = page * page_size;
    size_t end   = start + page_size;
    if (end > cvec_size(rows)) end = cvec_size(rows);

    for (size_t i = start; i < end; i++)
        json_append_row(resp, &cvec_at(rows, i));

    /* rows is destroyed automatically here regardless of which path was taken */
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
| `cvec_push(v, var)` | Append a copy of an lvalue; calls `fatal_err()` on failure |
| `cvec_push_rvalue(v, expr)` | Append an rvalue or expression (literal, computed value); calls `fatal_err()` on failure |
| `cvec_pop(v)` | Remove and return the last element as a value; calls `fatal_err()` if the vector is empty |
| `cvec_at(v, i)` | Return a modifiable lvalue reference to the element at index `i`; no bounds checking |
| `cvec_size(v)` | Return the number of elements currently stored |
| `cvec_reserve(v, n)` | Pre-allocate capacity for at least `n` elements; calls `fatal_err()` on failure |
| `cvec_data_ptr(v)` | Return a raw pointer to the internal data array; invalidated by any resize |

**Bulk Operations and Sorting**

| Macro | Description |
|---|---|
| `cvec_append_array(v, arr_ptr, count)` | Append `count` elements from a plain C array in a single operation; calls `fatal_err()` on failure |
| `cvec_append_cvec(v_dst, v_src)` | Append all elements of `v_src` to `v_dst`; both must have the same element type; calls `fatal_err()` on failure |
| `cvec_sort(v)` | Sort in place using the default comparator for the element type |
| `cvector_sort_with_comparison_proc(v, cmp)` | Sort in place using a caller-supplied comparator (`int cmp(const void *, const void *)`) |

---

## 6. Dynamic String - `cstring`

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

### Real-World Use Case: Parsing Configuration Files

A configuration reader that accepts `key = value` lines from a file illustrates the string manipulation API. Trimming handles inconsistent whitespace, splitting tokenises the line by delimiter, and `cstr_starts_with` skips comment lines cheaply:

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

## 7. Sorting - `csort`

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

### Real-World Use Case: Priority Job Queue

A background worker processes jobs in priority order while preserving submission order for jobs at the same priority level. The stable sort guarantee means equal-priority jobs are always dispatched in the order they arrived, without any secondary sort key:

```c
typedef struct {
    int      priority;    /* higher value = higher priority */
    uint64_t submit_ts;
    char     payload[128];
} Job;

int cmp_job_desc(const void *a, const void *b) {
    const Job *ja = (const Job *)a;
    const Job *jb = (const Job *)b;
    /* Descending: higher priority first */
    return (jb->priority > ja->priority) - (jb->priority < ja->priority);
}

void *job_getter(void *collection, size_t index) {
    return &((Job *)collection)[index];
}

void dispatch_next_batch(Job *queue, size_t count, size_t batch_size) {
    csort_sort(queue, count, sizeof(Job), job_getter, cmp_job_desc, NULL);

    size_t n = count < batch_size ? count : batch_size;
    for (size_t i = 0; i < n; i++)
        submit_to_thread_pool(&queue[i]);
}
```

Because `csort` is a stable sort, two jobs submitted at times `t1 < t2` with identical priority are always ordered `t1, t2` after sorting, regardless of how many sort passes have occurred.

---

## 8. Hash Map - `chashmap`

`chashmap` is an associative container with O(1) average-case insertion, lookup, and deletion. A distinctive feature is that it selects one of two internal implementations at compile time, based on the types of the key and value.

**Header:** `#include <chashmap.h>`

### Implementation Selection

**Open-addressing** is selected when both the key and the value are integral types no wider than eight bytes. It uses compact 17-byte slots (8-byte key, 8-byte value, 1-byte metadata), Fibonacci hashing for integers, and linear probing. Load factor thresholds are 0.70 (grow) and 0.25 (shrink), with a 2x scale factor. There are zero per-entry heap allocations, and cache locality is quite good.

**Separate chaining** is selected for all other type combinations. It uses a linked-list per bucket, XXHash64 for content-based hashing, Small String Optimisation (23-byte inline buffer for short strings), and a doubly-linked list that preserves insertion order. The minimum bucket count is 64 (always a power of two), and the scale factor is 4x.

The selection happens transparently; the same macro interface is used in both cases.

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

The unified iteration macros are provided by `citerators.h`, which is automatically included when you include `chashmap.h`. See [Section 10](#10-unified-iteration--citerators) for the full API reference.

Iteration order differs by implementation: separate chaining iterates in insertion order via its internal doubly-linked list; open-addressing iterates in slot order, which is neither insertion order nor sorted order.

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

### Real-World Use Case: HTTP Request Router

An HTTP router maps `"METHOD /path"` strings to handler function pointers. `chmap_get_ptr` provides O(1) dispatch and returns `NULL` for unregistered routes without triggering a fatal error:

```c
typedef void (*handler_fn)(HttpRequest *, HttpResponse *);

void router_init(chmap router) {
    chmap_redeclare(router, char*, handler_fn);

    handler_fn h;
    h = list_users_handler;   chmap_insert(router, "GET /users",       h);
    h = create_user_handler;  chmap_insert(router, "POST /users",      h);
    h = get_user_handler;     chmap_insert(router, "GET /users/:id",   h);
    h = delete_user_handler;  chmap_insert(router, "DELETE /users/:id", h);
}

void router_dispatch(chmap router, HttpRequest *req, HttpResponse *resp) {
    chmap_redeclare(router, char*, handler_fn);

    char key[128];
    snprintf(key, sizeof(key), "%s %s", req->method, req->path);

    handler_fn *fn = chmap_get_ptr(router, key);
    if (!fn) {
        resp->status = 404;
        return;
    }
    (*fn)(req, resp);
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

These macros come from `citerators.h`, which `chashmap.h` includes automatically. See [Section 10](#10-unified-iteration--citerators) for the full reference.

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

## 9. Ordered Map - `cbstmap`

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

A game server maintains a leaderboard where players are ranked by score. Storing negated scores as keys causes the AVL tree's ascending in-order traversal to visit entries from highest to lowest score. Insertion and lookup are both O(log n); the tree rebalances automatically:

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

These macros come from `citerators.h`, which `cbstmap.h` includes automatically. See [Section 10](#10-unified-iteration--citerators) for the full reference.

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

## 10. Unified Iteration - `citerators`

`citerators.h` provides a single, type-dispatched iteration API that works uniformly across `cvector`, `chashmap`, and `cbstmap`. There is no need to include it explicitly: each container header includes `citerators.h` at its own end, so any translation unit that includes a single container header automatically gets the full unified API.

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

### Real-World Use Case: Unified Audit Log Serializer

A function that serializes any `chmap` of string fields to a structured log line works without knowing the concrete field names at compile time. `chmap_redeclare` restores the companion type variables in the new scope, and `ccol_for_each` handles the rest:

```c
void audit_log_event(clog lg, const char *event, chmap fields) {
    chmap_redeclare(fields, char*, char*);

    cstr_construct_scoped(line, event);
    cstr_append(line, " ");

    ccol_for_each(fields, it, {
        cstr_append(line, *ccol_iter_key_ptr(it));
        cstr_append(line, "=");
        cstr_append(line, *ccol_iter_val_ptr(it));
        cstr_append(line, " ");
    });

    log_info(lg, "%s", cstr_c_str(line));
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

## 11. Memory Pools - `cmempool`

The library provides two pool allocators: a fixed-size pool (`mempool`) and a ranged pool (`r_mempool`). Both offer O(1) allocation and deallocation, optional thread safety, and an optional fallback to the system allocator when the pool is exhausted.

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

mempool *pool = r_mempool_create_from_preallocated_buffer(
    rmempool_buf, sizeof(rmempool_buf), 4, 12, 9,
    fallback_at_last_exhaustion, /*single_threaded=*/true,
    NULL, NULL);

void *slot = r_mempool_alloc_entry(pool, 100);
/* ... */
r_mempool_free_entry(slot);
r_mempool_destroy(pool);  /* The buffer itself is not freed */
```

### Real-World Use Case: Per-Connection Context Pool

A TCP server that handles thousands of short-lived connections benefits from a fixed-size pool for connection context structs. Allocation and deallocation are O(1) free-list operations with no heap fragmentation, and the fixed capacity bounds peak memory usage at startup:

```c
typedef struct {
    int      fd;
    char     peer_addr[46];
    uint64_t connect_ts;
} ConnCtx;

static mempool *conn_pool;

void server_init(int max_connections) {
    conn_pool = mempool_create(
        (size_t)max_connections,
        sizeof(ConnCtx),
        /*fallback=*/false,      /* return NULL instead of calling malloc */
        /*single_threaded=*/false,
        NULL, NULL);
}

ConnCtx *conn_accept(int fd, const char *peer) {
    ConnCtx *ctx = mempool_calloc_entry(conn_pool);   /* zero-initialised */
    if (!ctx) return NULL;                             /* pool exhausted */
    ctx->fd = fd;
    strncpy(ctx->peer_addr, peer, sizeof(ctx->peer_addr) - 1);
    ctx->connect_ts = now_us();
    return ctx;
}

void conn_close(ConnCtx *ctx) {
    close(ctx->fd);
    mempool_free_entry(ctx);   /* O(1) - returned to the free list */
}
```

Because every slot is the same size as `ConnCtx`, there is no fragmentation within the pool. Peak memory is fully determined by `max_connections * sizeof(ConnCtx)` - no surprises under load.

### Driving Other Containers from a Pool

Any container that accepts a `ccol_memmgmt_procs_t *` can be directed to allocate from a pool. See [Section 18](#18-custom-memory-management) for the complete pattern.

---

## 12. Thread Communication - `cthreadcomm`

The thread communication module provides three primitives for safe message passing between threads: a bounded circular queue, an unbounded dynamic queue, and a bidirectional channel. All three use a zero-copy ownership transfer model: the sender's pointer is set to `NULL` on a successful send, and the receiver becomes the sole owner of the data.

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

### Real-World Use Case: Async Image Processing Pipeline

A web server offloads image resizing to a pool of worker threads. The zero-copy ownership model means the heap-allocated job struct is never duplicated: `chan_send_zc` nulls the sender's pointer on success, and the worker becomes the sole owner:

```c
typedef struct {
    char path[256];
    int  target_width;
    int  target_height;
    char output_path[256];
} ImageJob;

void *image_worker(void *arg) {
    channel *ch = (channel *)arg;
    for (;;) {
        c_message_t msg;
        chan_recv_zc(ch, &msg);
        if (!msg.data) break;   /* NULL sentinel signals shutdown */

        ImageJob *job = (ImageJob *)msg.data;
        resize_image(job->path, job->target_width, job->target_height,
                     job->output_path);

        c_message_t ack = {
            .data = strdup(job->output_path),
            .size = strlen(job->output_path) + 1
        };
        free(job);
        chan_send_zc(ch, &ack);   /* transfer result back to dispatcher */
    }
    return NULL;
}

void dispatch_resize(channel *ch, const char *src, int w, int h,
                     const char *dst) {
    ImageJob *job = malloc(sizeof(ImageJob));
    snprintf(job->path,        sizeof(job->path),        "%s", src);
    snprintf(job->output_path, sizeof(job->output_path), "%s", dst);
    job->target_width  = w;
    job->target_height = h;

    c_message_t msg = { .data = job, .size = sizeof(*job) };
    chan_send_zc(ch, &msg);   /* job is now NULL - worker owns it */

    c_message_t ack;
    chan_recv_zc(ch, &ack);
    printf("done: %s\n", (char *)ack.data);
    free(ack.data);
}
```

The `channel` automatically routes sends and receives based on thread identity: the thread that called `channel_create` is the owner; all other threads are workers. No separate queue handles are required.

### Multiplexed Waiting - `ccol_select`

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
- A file descriptor read win: `msg.data` is heap-allocated by `ccol_select` (caller must `free`). `msg.size` is the byte count. EOF yields `msg.data = NULL`. The amount of data read per call depends on the fd type: datagram fds (SOCK_DGRAM / SOCK_SEQPACKET) receive one full datagram captured in a 66 KiB buffer; O_NONBLOCK stream / non-socket fds are fully drained until EAGAIN; blocking stream / non-socket fds receive exactly one read per call using a buffer of max(4096, max_bytes) bytes -- pass `max_bytes > 4096` via `selectable_from_fd_limited` to read more per call; remaining data is returned on the next `ccol_select` call (epoll is level-triggered -- use O_NONBLOCK if full-drain behaviour is required).
- A file descriptor write win: readiness is reported only; the caller then calls `write(2)`.
- Queue-only selectable sets use a condition variable path with no `epoll` overhead. Any file descriptor in the set switches the implementation to `epoll(7)` automatically.

---

## 13. LRU Cache - `clrucache`

`clrucache` is a fully thread-safe generic LRU (Least-Recently-Used) cache backed by a hash map for O(1) lookup and a doubly-linked list for O(1) eviction. When the cache is full, inserting a new entry evicts the least-recently-used live entry first, optionally notifying the caller via an eviction callback. An optional remote getter and setter integrate the cache transparently with an external backing store; a database, a network service, or any other source.

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

A remote getter is called on a cache miss. The cache takes ownership of the heap-allocated value returned by the getter. Concurrent requests for the same missing key coalesce: only one fetch executes, and all waiters receive the result.

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

### Real-World Use Case: Session Token Validation Cache

An authentication middleware validates bearer tokens on every request. Token validation involves a database round-trip on the first occurrence; subsequent requests for the same token are served from the cache in O(1). The remote getter coalesces concurrent misses for the same token, so only one database query executes even under a burst of parallel requests for an uncached token:

```c
/* Called automatically by the cache on a miss.
   Returns the user ID for the token, or 0 if the token is invalid. */
bool load_user_id(const cmap_pair *key_pair, cmap_pair *val_pair) {
    const char *token = (const char *)key_pair->ptr;

    long *uid = malloc(sizeof(long));
    if (!uid) return false;

    *uid = db_validate_token(token);
    if (*uid == 0) { free(uid); return false; }

    val_pair->ptr  = uid;
    val_pair->size = sizeof(long);
    return true;
}

/* Module-level cache: hold the 8192 most recently validated tokens */
clru_construct(token_cache, char*, long, 8192, load_user_id, NULL, NULL);

/* Called from multiple threads on every inbound request */
int auth_middleware(const char *bearer_token) {
    clru_redeclare(token_cache, char*, long);

    char *tok = (char *)bearer_token;
    long uid  = 0;
    if (clru_get(token_cache, tok, &uid) != ccol_success)
        return 401;

    attach_user_context(uid);
    return 200;
}
```

The LRU eviction policy bounds memory usage: the 8192 most recently validated tokens stay hot in memory; older ones are evicted silently. The coalescing property means that a sudden spike of requests for an uncached token causes exactly one database query rather than a thundering herd.

### Memory Ownership for Retrieved Values

`clru_get` memory behavior depends on the value type. Only `char *` values cause a heap allocation; for all other types no heap allocation occurs.

**Non-`char *` value types** - the macro copies the value directly into `*val_ptr` with no heap allocation. The caller receives the value in a plain typed variable; `free()` is neither needed nor valid:

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
| `clru_destroy(name)` | Destroy the cache and set the pointer to `NULL` |
| `clrucache_size(cache)` | Return the number of live entries currently stored |
| `clrucache_capacity(cache)` | Return the configured capacity |

**Get / Set**

| Macro / Function | Description |
|---|---|
| `clru_get(name, key, val_ptr)` | Retrieve the value for `key`. `val_ptr` is a pointer to the value type (`ValT *`), **not** `cmap_pair *`. For non-`char *` val types, the value is copied directly into `*val_ptr`; no heap allocation occurs. For `char *` val types, `*(char **)val_ptr` is set to a heap-allocated string allocated by the cache's custom allocator (or `malloc()` if none was configured); the caller must free it with the matching function. Returns `ccol_success`, `ccol_key_not_found`, or another error code. |
| `clru_set(name, key, val)` | Store `val` for `key`; if a remote setter was provided it is called first; returns `ccol_success` or `ccol_unexpected_failure` on remote failure |

---

## 14. Structured Logger - `clogger`

`clogger` is a thread-safe, structured logger with three output formats: logfmt (default), NDJSON, and RFC 5424 syslog. Each record is machine-parseable and human-readable. A single per-logger `pthread_mutex_t` serialises all writes and state changes.

**Header:** `#include <clogger.h>`

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

#### JSON (NDJSON)

When `CLOG_FMT_JSON` is selected, each log record is a single self-contained JSON object followed by a newline:

```json
{"ts":"2026-05-29T21:52:39.096473Z","level":"INFO","proc":"myapp(1234):main(1234)","src":"main.c:9","func":"main","env":"prod","msg":"starting up"}
{"ts":"2026-05-29T21:52:39.096642Z","level":"ERROR","proc":"myapp(1234):main(1234)","src":"main.c:10","func":"main","env":"prod","msg":"db failed","bt":["#0 main+0x16d","#1 libc.so.6+0x29ca8"]}
```

Structured fields appear as top-level JSON keys (insertion order). For `log_error`, `log_alert`, and `log_fatal` the backtrace is embedded as a `"bt"` string array inside the same JSON object instead of being written as separate continuation lines.

#### Syslog (RFC 5424)

When `CLOG_FMT_SYSLOG` is selected, each record is emitted as a single RFC 5424 message:

```
<PRI>1 TIMESTAMP HOSTNAME APP-NAME PID MSGID [ccol proc="name(pid):tname(tid)" src="file:N" func="fn" [fields]] MSG
```

The `PRI` field encodes both the facility (default `CLOG_SYSLOG_USER`; see `clog_set_facility`) and the severity level mapped from `clog_level_t` according to RFC 5424 (TRACE/DEBUG -> 7, INFO -> 6, WARN -> 4, ERROR -> 3, ALERT -> 1, FATAL -> 0). For `log_error`, `log_alert`, and `log_fatal` each backtrace frame is emitted as a separate syslog message carrying the same PRI and MSGID.

`CLOG_FMT_SYSLOG` is restricted to **fd-based loggers** (`clog_open_fd` / `clog_open_fd_mp`). Calling `clog_set_format` with `CLOG_FMT_SYSLOG` on a file-backed logger is a silent no-op. The fd must be connected to a syslog daemon beforehand; on Linux this is typically a `SOCK_DGRAM` Unix socket at `/dev/log`:

```c
int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
struct sockaddr_un sa = { .sun_family = AF_UNIX };
strncpy(sa.sun_path, "/dev/log", sizeof sa.sun_path - 1);
connect(fd, (struct sockaddr *)&sa, sizeof sa);

clog lg = clog_open_fd(fd, CLOG_INFO);
clog_set_format(lg, CLOG_FMT_SYSLOG);
clog_set_facility(lg, CLOG_SYSLOG_DAEMON);

log_info(lg, "service started");
clog_close(lg);
close(fd);
```

Keep individual messages under 2 KiB to stay within typical syslogd datagram limits. Log rotation is unavailable for fd-based loggers.

---

The format is stored on the **shared backing store**, so it applies to all logger handles that write to the same file descriptor (root and every derived logger). Setting it via any handle takes effect immediately for all of them:

```c
clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, NULL, NULL);
clog_set_format(lg, CLOG_FMT_JSON);

clog derived = clog_derive(lg);
/* derived also writes JSON - it shares the same backing store */

clog_format_t fmt = clog_get_format(lg); /* CLOG_FMT_JSON */
```

Backtrace (for any format) requires linking the binary with `-rdynamic`. On platforms without `execinfo.h` (glibc, macOS, FreeBSD) the backtrace is omitted silently.

### Basic Usage

```c
#include <clogger.h>

/* Logger targeting stderr */
clog lg = clog_open_fd_mp(2, CLOG_INFO, NULL);

/* Attach persistent fields */
clog_set_field(lg, "env",     "prod");
clog_set_field(lg, "service", "auth");

log_info(lg,  "starting up");
log_warn(lg,  "config missing: %s", "timeout");
log_error(lg, "db failed: %s", "timeout");   /* also appends a backtrace; log_alert and log_fatal do too */

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

clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, &cfg, NULL);
```

When a rotation fires the current file is renamed to `<path>.<YYYYMMDDHHMMSS>` (e.g. `app.log.20260529215239`) and a new file is opened. Collisions within the same second are resolved with a `_1`, `_2`, ... suffix. When `max_rotated_files` is positive, the oldest rotated files beyond the limit are deleted automatically.

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
clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, NULL, &my_alloc);
```

All four function pointers must be set; passing a partially-populated struct returns `NULL`.

### Log Levels

| Level | Value | Notes |
|---|---|---|
| `CLOG_TRACE` | 0 | Finest-grained detail |
| `CLOG_DEBUG` | 1 | |
| `CLOG_INFO`  | 2 | Recommended production minimum |
| `CLOG_WARN`  | 3 | |
| `CLOG_ERROR` | 4 | Appends a backtrace |
| `CLOG_ALERT` | 5 | Action required immediately; maps to RFC 5424 severity 1; appends a backtrace |
| `CLOG_FATAL` | 6 | Appends a backtrace; does not terminate the process |
| `CLOG_OFF`   | 7 | Disables all output when used as `min_level` |

The minimum level can be changed at any time with `clog_set_level`. Messages below the current minimum are dropped without acquiring the mutex.

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

### NULL Safety

Passing `NULL` as the logger to any macro or function is always a no-op. This allows loggers to be silenced at runtime without touching every call site:

```c
clog lg = production_mode ? clog_open_fd_mp(2, CLOG_INFO, NULL) : NULL;
log_info(lg, "this line is discarded when lg is NULL");
clog_close(lg);   /* safe */
```

### Real-World Use Case: Per-Request Structured Logging

A service logs every inbound request with a unique request ID and the authenticated user. Using `clog_derive`, each request handler gets a private logger that shares the underlying file and mutex with the root logger but carries its own request-scoped fields. The derived handle is released when the request completes without affecting the root or any other derived loggers:

```c
clog g_logger;   /* root logger, initialised at startup */

void handle_request(const char *req_id, long uid, const char *path) {
    clog req_log = clog_derive(g_logger);

    char uid_buf[32];
    snprintf(uid_buf, sizeof(uid_buf), "%ld", uid);

    clog_set_field(req_log, "request_id", req_id);
    clog_set_field(req_log, "user_id",    uid_buf);
    clog_set_field(req_log, "path",       path);

    log_info(req_log, "request received");

    int rc = process_request(path);
    if (rc != 0)
        log_error(req_log, "request failed with code %d", rc);
    else
        log_info(req_log, "request completed");

    clog_close(req_log);   /* this handle is released; root logger is unaffected */
}

int main(void) {
    clog_rotation_cfg_t rot = {
        .size_rotation_enabled  = true,
        .max_file_size          = 100L * 1024L * 1024L,   /* 100 MiB */
        .time_rotation_enabled  = true,
        .rotation_interval_secs = 86400,                   /* daily */
        .max_rotated_files      = 30,
    };
    g_logger = clog_open_file_mp("/var/log/myservice.log", CLOG_INFO, &rot, NULL);
    clog_set_field(g_logger, "service", "myservice");
    clog_set_field(g_logger, "env",     "prod");

    /* ... accept and dispatch requests ... */

    clog_close(g_logger);
    return 0;
}
```

Request-scoped fields (`request_id`, `user_id`, `path`) appear in every line emitted by `req_log` but are absent from lines emitted by the root logger or any other derived logger. All writes share a single mutex, so output from concurrent requests is never interleaved.

### Reference: Core Operations

**Lifecycle**

| Function | Description |
|---|---|
| `clog_open_fd_mp(fd, min_level, mprocs)` | Create a logger writing to an existing open fd; the fd is not closed on `clog_close` |
| `clog_open_file_mp(path, min_level, cfg, mprocs)` | Create a file-backed logger; pass a `clog_rotation_cfg_t *` for rotation or `NULL` to disable it |
| `clog_derive(parent)` | Create a derived logger sharing the same fd, mutex, and rotation as `parent`; starts with a snapshot of `parent`'s fields and level, then evolves independently; release with `clog_close` |
| `clog_close(logger)` | Flush, close (if file-backed), and free all resources; the underlying fd is kept open until all derived handles are also closed; safe with `NULL` |

**Level Control**

| Function | Description |
|---|---|
| `clog_set_level(logger, level)` | Change the minimum log level; thread-safe |
| `clog_get_level(logger)` | Return the current minimum level; returns `CLOG_OFF` for a `NULL` logger |

**Output Format and Syslog Facility**

| Function | Description |
|---|---|
| `clog_set_format(logger, fmt)` | Change the output format (`CLOG_FMT_LOGFMT`, `CLOG_FMT_JSON`, or `CLOG_FMT_SYSLOG`); affects all handles sharing the same fd; `CLOG_FMT_SYSLOG` is a no-op on file-backed loggers; thread-safe |
| `clog_get_format(logger)` | Return the current format; returns `CLOG_FMT_LOGFMT` for a `NULL` logger |
| `clog_set_facility(logger, facility)` | Change the RFC 5424 syslog facility (e.g. `CLOG_SYSLOG_DAEMON`); affects all handles sharing the same fd; only meaningful with `CLOG_FMT_SYSLOG`; default is `CLOG_SYSLOG_USER`; thread-safe |
| `clog_get_facility(logger)` | Return the current syslog facility; returns `CLOG_SYSLOG_USER` for a `NULL` logger |

**Structured Fields**

| Function | Description |
|---|---|
| `clog_set_field(logger, key, value)` | Attach a persistent `key=value` field; updates the value if the key already exists |
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
| `log_fatal(lg, fmt, ...)` | `CLOG_FATAL` | Yes - does not terminate the process; caller must call `abort()` / `exit()` |

---

## 15. JSON Parser / Serializer / DOM - `cjson`

`cjson` provides a fully mutable JSON Document Object Model (DOM), a recursive-descent parser, a serializer (compact and pretty-print), and two type-safe path macros (`cjson_get` and `cjson_set`) for reading and writing anywhere in the tree without chaining individual lookup calls.

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

### Serialization

```c
/* All of the pointers returned by cjson_serialize* variants
   need to be freed by the callers */
char *compact = cjson_serialize(doc);           /* compact serialization */
char *pretty  = cjson_serialize_pretty(doc, 2); /* 2-space indent */
cjson_serialize_free(compact);
cjson_serialize_free(pretty);

compact = cjson_serialize_mp(doc, mp);          /* compact using custom memory procs */
pretty = cjson_serialize_pretty_mp(doc, 2, mp); /* 2-space indent using custom memory procs */
cjson_serialize_free_mp(compact, mp);
cjson_serialize_free_mp(pretty, mp);
```

### Path navigation - `cjson_get` and `cjson_set`

Paths are dot-separated component strings.  A component that begins with `#` followed by **one or more decimal digits** addresses **an array element by index when the current node is an array**; otherwise it is treated as a **literal object key**.  A bare `#` with no trailing digits is always an error (`cjson_get` returns `NULL`; `cjson_set` returns `ccol_invalid_args`).

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

`cjson_set` accepts: `bool`, any integer type, `float`, `double`, `char *`, `const char *`, and string literals.  It detects the C type at compile time via `_Generic` and routes to the correct storage path.  Passing an untyped `NULL` literal sets the leaf to `CJSON_NULL`; a typed null pointer such as `(const char *)NULL` also produces `CJSON_NULL` because a null C string pointer maps to JSON null.  Non-finite `double` values (`INFINITY`, `-INFINITY`, `NAN`) are rejected and `ccol_invalid_args` is returned; the existing node is left untouched.  Signed integer types (including plain `char` on platforms where `char` is signed, e.g. x86-64 Linux) are sign-extended correctly to `long long`.

Duplicate object keys set within the JSON object use **last-value-wins** semantics; the final occurrence of a key is retained and prior occurrences are deep-freed.

### Real-World Use Case: Webhook Payload Normalization

A webhook receiver must validate an incoming JSON payload, normalize a status field, inject a server-assigned timestamp, and forward the updated document downstream. The `cjson_get` and `cjson_set` path macros allow targeted updates deep in the tree without rebuilding the whole document:

```c
char *normalize_webhook(const char *raw_payload) {
    char *err = NULL;
    cjson doc = cjson_parse(raw_payload, &err);
    if (!doc) {
        fprintf(stderr, "parse error: %s\n", err);
        free(err);
        return NULL;
    }

    /* Validate a required field */
    cjson event_type = cjson_get(doc, "event.type");
    if (!event_type || cjson_type(event_type) != CJSON_STRING) {
        cjson_destroy(doc);
        return NULL;
    }

    /* Overwrite the status field in place - old node is deep-freed automatically */
    cjson_set(doc, "event.status", "received");

    /* Inject server-side metadata */
    cjson_set(doc, "meta.processed_at", (long long)time(NULL));
    cjson_set(doc, "meta.processor",    "gateway-v2");

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

- `cjson_list_push` **transfers ownership unconditionally**: on success the array owns the child; on failure `cjson_list_push` deep-frees it.  Do not free the child after calling this function regardless of the return value.
- `cjson_dictionary_set` **transfers ownership unconditionally**: on success the object owns the child; on failure `cjson_dictionary_set` deep-frees it.  Do not free the child after calling this function regardless of the return value.
- `cjson_get` returns a **non-owning** reference valid until the tree is mutated or destroyed.
- `cjson_clone` returns a fully independent deep copy.

### Custom memory management

Every factory and parse function has an `_mp` variant that accepts a `ccol_memmgmt_procs_t *mp` parameter.  The names without `_mp` suffix are `static inline` wrappers that pass `NULL` (default `malloc`/`free`/`calloc`/`realloc`).

```c
/* All nodes in the tree use my_procs. */
char *err = NULL;
cjson doc = cjson_parse_mp(json_str, &err, &my_procs);
if (!doc) { fprintf(stderr, "%s\n", err); free(err); /* handle error */ }
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

`cyaml` provides a fully mutable YAML Document Object Model (DOM), a hand-written recursive-descent parser, a block serializer, a compact flow serializer, and two type-safe path macros (`cyaml_get` and `cyaml_set`) for reading and writing anywhere in the tree without chaining individual lookup calls.

### Supported YAML features

| Feature | Notes |
|---|---|
| Block mappings | Indentation-sensitive key: value pairs |
| Block sequences | Indentation-sensitive `- item` lists |
| Flow mappings | `{key: value, ...}` inline style |
| Flow sequences | `[a, b, c]` inline style |
| Plain scalars | Unquoted values |
| Single-quoted scalars | No escape processing; `''` encodes a literal `'` |
| Double-quoted scalars | Full YAML escape sequences including `\uXXXX` |
| Literal block scalars | `|` -- newlines preserved |
| Folded block scalars | `>` -- newlines folded to spaces except blank lines |
| Block scalar chomping | `|+` keep, `|-` strip, `|` clip (default) |
| Anchors and aliases | `&name` / `*name`; aliases resolve to deep clones; scoped per document |
| YAML 1.2 core schema | Implicit type resolution for null/bool/int/float |
| Leading `---` / trailing `...` | Document-start and document-end markers |
| Multi-document streams | Supported; see below |
| Comments | `#` to end-of-line; silently ignored |
| Tags | `!tag` / `!!tag`; silently ignored |

**Not supported:** multi-line plain scalars (use `|` or `>` instead).

### Multi-document streams

When the input contains multiple `---`-delimited documents, `cyaml_parse` (and its variants) returns a single `CYAML_LIST` node whose elements are the individual document roots in order.  A single-document input is always returned as its root node directly -- no wrapping list.

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
- Every subsequent document **must** begin with `---`.
- An optional `...` end marker may follow each document.
- Anchors are scoped per document; a `*alias` cannot reference an `&anchor`
  from a different document.
- An empty document (e.g. two consecutive `---` markers) yields a `CYAML_NULL`
  element in the list.

### Node types

Every YAML value is represented by an opaque `cyaml` handle.  The type tag is a `cyaml_node_type_t` enum:

| Constant | Internal storage | YAML kind |
|---|---|---|
| `CYAML_NULL` | -- | `null`, `~`, or empty value |
| `CYAML_BOOL` | `bool` | `true` / `false` and case variants |
| `CYAML_INTEGER` | `long long` | Decimal, `0x` hex, `0o` octal |
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
/* Block style -- human-readable YAML */
char *block = cyaml_serialize(doc);

/* Flow style -- compact single-line YAML */
char *flow = cyaml_serialize_flow(doc);

/* Both return heap-allocated strings that must be freed. */
cyaml_serialize_free(block);
cyaml_serialize_free(flow);

/* cyaml_serialize() uses the node's own stored allocator automatically.
 * When the tree was created with a custom allocator, free with _mp: */
char *block2 = cyaml_serialize(doc);
cyaml_serialize_free_mp(block2, mp);
```

### Path navigation -- `cyaml_get` and `cyaml_set`

Paths are dot-separated component strings.  A component that begins with `#` followed by one or more decimal digits addresses a sequence element by index when the current node is a sequence; otherwise it is treated as a literal mapping key.

Two escape sequences are recognised inside path strings:

| Sequence | Meaning in key |
|----------|----------------|
| `\\.`    | A literal `.` character (not a path separator) |
| `\\\\`   | A literal `\` character |

A `\` before any other character is passed through unchanged.

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

### Deleting nodes -- `cyaml_delete`

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

Aliases resolve to independent deep clones of the anchored node.  Modifying the alias does not affect the original.  Merge keys (`<<:`) are not supported; see the "Not supported" note above.

### Lifecycle

```c
/* Manual destroy: */
cyaml_destroy(doc);    /* frees all nodes; NULLs the pointer */

/* RAII (GCC/Clang only): */
cyaml_declare_scoped(doc2) = cyaml_parse("x: 1\n", NULL);
/* doc2 is automatically freed when it goes out of scope */
```

### Custom allocators

```c
cyaml_declare(doc);
doc = cyaml_parse_mp(yaml, &err, &my_mprocs);
cyaml_dictionary_set(doc, "key", cyaml_create_string_mp("val", &my_mprocs));
cyaml_destroy(doc);
```

The allocator is stamped on every node at creation time.  `cyaml_destroy()` uses each node's own stored allocator.  `cyaml_clone()` inherits the allocator from the source tree.

**Node pool:** `cyaml` maintains a per-thread free-list (capped at 512 nodes) to amortize allocation cost.  Custom-allocator nodes bypass the pool entirely.  Default-allocator nodes use the pool; it is drained at thread exit with plain `free()`.

---

## 17. Thread Safety

The library applies a consistent policy: **components that pass data between threads or provide shared services carry their own synchronisation; components used for single-threaded data manipulation are deliberately unguarded.**

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

The library provides thin, portable wrappers over pthreads in `include/common.h`:

```c
/* Exclusive mutex */
mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
mutex_lock(lock);
chmap_insert(map, key, value);
mutex_unlock(lock);

/* Reader-writer lock for read-heavy workloads */
rw_lock_t rw = PTHREAD_RWLOCK_INITIALIZER;

rw_lock_rdlock(rw);
int val = chmap_get(map, key);
rw_lock_unlock(rw);

rw_lock_wrlock(rw);
chmap_insert(map, key, new_value);
rw_lock_unlock(rw);
```

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

### Per-Component Constraints

**`clrucache` eviction callback.** The callback passed to `clru_construct` is invoked **while the cache mutex is held**. It must not call back into the same cache handle, doing so will deadlock. It may allocate memory or write to a logger, but must not call `clru_get` or `clru_set` on the cache that triggered the eviction.

**`clogger` derived loggers.** `clog_derive` creates a sibling logger that shares the same fd, rotation state, and mutex as the root logger via the shared backing store. Writes from the root and all of its siblings are fully serialised with no additional locking required at the call site. The minimum-level check (`log_info`, `log_warn`, and similar macros) reads the per-logger level field without holding the mutex as a deliberate performance optimisation; a concurrent `clog_set_level` may therefore cause a single message near the boundary level to be inconsistently logged or dropped. This is intentional: the optimisation avoids mutex acquisition for every suppressed message, and the inconsistency window is not a data-corruption hazard.

---

## 18. Custom Memory Management

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

## 19. License

MIT License

Copyright (c) 2026 - C Collections Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
