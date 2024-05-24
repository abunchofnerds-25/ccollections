# C Collections Library

A production-ready collection of generic data structures and utilities for C, providing type-safe interfaces through extensive use of C11 features and GNU C extensions.

## Overview

This library provides fundamental data structures with a focus on:

- **Type safety** through C11 `_Generic` and compile-time type introspection
- **Memory efficiency** with optimized implementations and custom memory management support
- **Developer ergonomics** via intuitive macro-based APIs
- **Thread-safety options** for concurrent applications
- **Zero dependencies** beyond standard C library and pthreads

The library is compiled as `libccollections.so` which includes:

- **Dynamic arrays** (vectors) with automatic resizing
- **Hash maps** with dual implementation strategy (open-addressing and separate chaining) depending on key and value types
- **Ordered maps** using self-balancing AVL trees
- **Sorting utilities** with stable mergesort
- **Memory pools** (fixed-size and ranged)
- **Thread communication** primitives for message passing

## Table of Contents

- [Installation](#installation)
- [Quick Start](#quick-start)
- [Core Concepts](#core-concepts)
- [Data Structures](#data-structures)
  - [Vector (cvector)](#vector-cvector)
  - [Hash Map (chashmap)](#hash-map-chashmap)
  - [Binary Search Tree Map (cbstmap)](#binary-search-tree-map-cbstmap)
  - [Sorting (csort)](#sorting-csort)
  - [Memory Pools (cmempool)](#memory-pools-cmempool)
  - [Thread Communication (cthreadcomm)](#thread-communication-cthreadcomm)
- [API Conventions](#api-conventions)
- [Thread Safety](#thread-safety)
- [Memory Management](#memory-management)
- [License](#license)

## Installation

Link against `libccollections` and include the appropriate headers:

```c
#include <cvector.h>
#include <chashmap.h>
#include <cbstmap.h>
#include <csort.h>
#include <cmempool.h>
#include <cthreadcomm.h>
```

Compilation:
```bash
gcc -o myapp myapp.c -lccollections -lpthread
```

## Quick Start

```c
#include <cvector.h>
#include <chashmap.h>
#include <stdio.h>

int main(void) {
    // Create a vector of integers
    cvec_construct(numbers, int);
    
    // Add elements
    cvec_push_rvalue(numbers, 42);
    cvec_push_rvalue(numbers, 17);
    cvec_push_rvalue(numbers, 8);
    
    // Sort the vector
    cvec_sort(numbers);
    
    // Access elements
    printf("First element: %d\n", cvec_at(numbers, 0));
    
    // Clean up
    cvec_destroy(numbers);
    
    // Create a scoped hash map: string -> int
    // which does not need to be destroyed, since
    // it'll be automatically destroyed when its scope
    // ceases to exist.
    chmap_construct_scoped(ages, char*, int);
    
    // Insert key-value pairs (use variables)
    char *name1 = "Alice";
    int age1 = 30;
    chmap_insert(ages, name1, age1);
    
    char *name2 = "Bob";
    int age2 = 25;
    chmap_insert(ages, name2, age2);
    
    // Retrieve values
    char *lookup = "Alice";
    int age = chmap_get(ages, lookup);
    printf("Alice's age: %d\n", age);
    
    // Clean up not required
    // chmap_destroy(ages);
    
    return 0;
}
```

## Core Concepts

### Convenience Macro Patterns

The library provides several macro patterns for ease of use:

#### Declaration and Initialization

Most containers support three macro patterns:

1. **`*_declare`** - Declares the container variable
2. **`*_init`** - Initializes an already-declared container
3. **`*_construct`** - Combines declaration and initialization in one step
4. **`*_redeclare`** - Re-enables the usage of type-safe macros in a new function

Example:
```c
// Pattern 1: Separate declaration and initialization
cvec_declare(vec1, int);
// "cvec_declare_scoped(vec1, int);" would declare an auto-cleaning vector
cvec_init(vec1);

// Pattern 2: Combined declaration and initialization
cvec_construct(vec2, int);  // Equivalent to declare + init
// "cvec_construct_scoped(vec2, int);" would construct an auto-cleaning vector

// Pattern 3: With custom memory management
cvec_construct_mp(vec3, int, my_mprocs);
// "cvec_construct_mp_scoped(vec3, int, my_mprocs);" would also construct an auto-cleaning vector
```

**Note:** `*_construct` macros are simply convenience wrappers that combine `*_declare` and `*_init` - use whichever pattern fits your code style.

#### Re-declaring for Function Parameters

When you pass containers to functions, the type information needed by the convenience macros is lost. Use `*_redeclare` macros to restore type-safe operations:

```c
// Function that receives a vector
void process_vector(cvec vec) {
    // Re-establish type information for this function scope
    cvec_redeclare(vec, int);
    
    // Now type-safe macros work
    cvec_push_rvalue(vec, 42);
    int val = cvec_at(vec, 0);
    cvec_sort(vec);
}

// Similarly for maps
void process_map(chmap map) {
    chmap_redeclare(map, int, char*);
    
    int key = 1;
    char *value = "hello";
    chmap_insert(map, key, value);
    
    int lookup = 1;
    char *val = chmap_get(map, lookup);
}

void process_bst(cbmap tree) {
    cbmap_redeclare(tree, int, char*);
    
    int key = 1;
    char *value = "world";
    cbmap_insert(tree, key, value);
}

int main(void) {
    cvec_construct(numbers, int);
    chmap_construct(map, int, char*);
    cbmap_construct(tree, int, char*);
    
    process_vector(numbers);
    process_map(map);
    process_bst(tree);
    
    cvec_destroy(numbers);
    chmap_destroy(map);
    cbmap_destroy(tree);
    
    return 0;
}
```

**Why is this needed?** The type-safe macros rely on hidden type variables created by `*_declare`/`*_construct`. When you pass a container to a function, these variables aren't available in the new scope. `*_redeclare` creates new type variables that reference the same container instance. These mysterious variables are just simple pointers, by the way.

#### Scoped Auto-Cleanup

**All containers** support **`*_scoped`** variants that automatically clean up when going out of scope (requires GNU C `cleanup` attribute):

```c
void process_data(void) {
    cvec_construct_scoped(temp_data, int);
    
    // Use temp_data...
    cvec_push_rvalue(temp_data, 42);
    
    // Automatically destroyed when leaving the scope
}
```

This is particularly useful for error handling paths where manual cleanup becomes cumbersome and error-prone.

### Type Safety and RValues

The library uses C11 `_Generic` for compile-time type introspection, providing type-safe operations without runtime overhead.

#### Vectors and RValues

Vectors provide special handling for rvalue expressions:

```c
cvec_construct(vec, int);

int x = 42;
cvec_push(vec, x);              // For lvalues (addressable variables)
cvec_push_rvalue(vec, 42);      // For rvalues (literals, expressions)
cvec_push_rvalue(vec, x + 10);  // For computed values
```

#### Maps and Variable Requirement

**Important limitation:** Due to C's constraints, convenience macros of the map implementations (`chmap` and `cbmap`)
cannot directly consume rvalue expressions or literal constants (except string literals). You **must always** use variables:

```c
chmap_construct(map, int, char*);

// Correct - using variables
int key = 42;
char *value = "hello";
chmap_insert(map, key, value);

// INCORRECT - literals/constants don't work
chmap_insert(map, 42, "world");  // ERROR: cannot take address of constant

// For literals, assign to variables first
int key2 = 1;
char *value2 = "world";
chmap_insert(map, key2, value2);

// String literals do not have that limitation
int key3 = 2;
chmap_insert(map, key3, "foo");

// For complex expressions, use temporary variables
int computed_key = expensive_calculation();
char *computed_value = build_string();
chmap_insert(map, computed_key, computed_value);
```

This limitation exists because the map insert macros take addresses of the key and value parameters using the `&` operator. Since constants and rvalue expressions have no addressable storage, this operation fails. Always store your keys and values in variables before passing them to map operations.

### Return Values

Most operations return `ccol_retval_t` status codes:

```c
typedef enum {
  ccol_unexpected_failure,      /**< -9: Unexpected/unknown error */
  ccol_container_empty,         /**< -8: Container has no elements */
  ccol_container_full,          /**< -7: Container at maximum capacity */
  ccol_timed_out,               /**< -6: Operation timed out */
  ccol_not_permitted,           /**< -5: Operation not allowed in current state */
  ccol_invalid_args,            /**< -4: Invalid arguments provided */
  ccol_key_not_found,           /**< -3: Key does not exist in map */
  ccol_key_already_present,     /**< -2: Key already exists (for update operations) */
  ccol_not_enough_memory,       /**< -1: Memory allocation failed */
  ccol_success                  /**<  0: Operation succeeded */
} ccol_retval_t;
```

The majority of the convenience macros call `fatal_err()` on a non-recoverable failure, terminating the program (When they detect a caller bug or when there is a shortage of resources). For finer control, use the underlying functions directly.

## Data Structures

### Vector (cvector)

Dynamic array with automatic resizing.

#### Features

- Amortized O(1) push and pop operations
- Minimum capacity of 4 elements
- Grows by 2× when full, shrinks by 0.5× when less than 1/4 occupied
- Stable sorting via mergesort integration
- Type-safe element access

#### Basic Operations

```c
// Creation
cvec_construct(vec, int);
cvec_construct_mp(vec_custom, double, my_mprocs);  // Custom memory management
cvec_construct_scoped(temp, int);                  // Auto-cleanup

// Adding elements
int x = 10;
cvec_push(vec, x);              // Push lvalue
cvec_push_rvalue(vec, 42);      // Push rvalue/literal

// Accessing elements
int first = cvec_at(vec, 0);         // Returns reference to the value (Dereferenced pointer)
cvec_at(vec, 0) = 3;                 // Also modifies in place

// Size and capacity
size_t count = cvec_size(vec);

// Iteration
// Index-based (traditional)
for (size_t i = 0; i < cvec_size(vec); i++) {
    printf("%d ", cvec_at(vec, i));
    cvec_at(vec, i) += 1; // Can also be used for in place modifications
}

// Removing elements
int last = cvec_pop(vec);  // Returns value

// Sorting
cvec_sort(vec);                              // Default comparison
cvec_sort_with_comparison_proc(vec, my_cmp); // Custom comparison

// Cleanup
cvec_reset(vec);      // Clear all elements, reset capacity
cvec_destroy(vec);    // Destroy and set to NULL
```

#### Example: Processing Data

```c
cvec_construct(temperatures, double);

// Collect temperature readings
cvec_push_rvalue(temperatures, 23.5);
cvec_push_rvalue(temperatures, 21.8);
cvec_push_rvalue(temperatures, 24.2);

// Sort the data
cvec_sort(temperatures);

// Calculate median
size_t n = cvec_size(temperatures);
double median = cvec_at(temperatures, n / 2);

printf("Median temperature: %.1f°C\n", median);

cvec_destroy(temperatures);
```

### Hash Map (chashmap)

Dictionary/associative array with O(1) average-case operations.

#### Features

The hash map automatically selects between two implementation strategies based on key and value types:

**Open-Addressing** (for integral types ≤8 bytes for both key and value):
- Compact 17-byte slots (8-byte key + 8-byte value + 1-byte metadata)
- Linear probing with Fibonacci hashing for integers
- Load factor thresholds: 0.70 (grow) / 0.25 (shrink)
- Zero per-entry allocations
- Excellent cache locality

**Separate Chaining** (for non-integral types or types >8 bytes):
- Linked lists for collision resolution
- Small String Optimization: 23-byte inline storage
- Doubly-linked list maintains insertion order
- Minimum 64 buckets (always power-of-2)
- Scale factor: 4× (grows to 4×, shrinks to 0.25×)

Both implementations:
- Default hashing: XXHash64 for buffers, Fibonacci for integers
- Custom hashing function support
- Automatic resizing based on load factor

#### Basic Operations

```c
// Creation - implementation chosen automatically based on types
chmap_construct(map, int, char*);           // Separate chaining (string value)
chmap_construct(numbers, int, int);         // Open-addressing (both integral ≤8 bytes)
chmap_construct(cache, char*, double);      // Separate chaining (string key)

// With custom settings
chmap_construct_full(map, char*, int, 128, my_mprocs, my_hash_func);

// Insertion (always use variables - constants and rvalues don't work)
int key = 42;
char *value = "hello";
chmap_insert(map, key, value);

// Multiple insertions
int key2 = 100;
char *value2 = "world";
chmap_insert(map, key2, value2);

// Retrieval
int lookup_key = 42;
char *val = chmap_get(map, lookup_key);          // Returns value, fatal_err if not found
char **ptr = chmap_get_ptr(map, lookup_key);     // Returns pointer or NULL

// Check existence
if (chmap_get_ptr(map, lookup_key) != NULL) {
    printf("Key exists\n");
}

// Removal
int remove_key = 42;
ccol_retval_t result = chmap_remove(map, remove_key);
if (result == ccol_key_not_found) {
    printf("Key not found\n");
}

// Iteration - RECOMMENDED METHOD
// The chmap_for_each macro is the standard way to iterate
chmap_for_each(map, it, {
    int k = *chmap_iter_key_ptr(it);
    char *v = *chmap_iter_val_ptr(it);
    printf("Key: %d, Value: %s\n", k, v);
});

// Manual iteration (if you need more control)
// Please notice that the iterator has to be declared outside
// of the for loop. That's why the chmap_for_each exists in
// the first place.
chmap_iter_declare(map, it);
for (it = chmap_begin(map); it != NULL; it = chmap_iter_next(it)) {
    int k = *chmap_iter_key_ptr(it);
    char *v = *chmap_iter_val_ptr(it);
    printf("Key: %d, Value: %s\n", k, v);
}

// Cleanup
chmap_reset(map);     // Remove all entries
chmap_destroy(map);   // Destroy and set to NULL
```

#### Example: Word Frequency Counter

```c
chmap_construct(word_count, char*, int);

// Count word occurrences
const char *words[] = {"hello", "world", "hello", "foo", "world", "hello"};
for (size_t i = 0; i < 6; i++) {
    char *word = (char*)words[i];
    int *count_ptr = chmap_get_ptr(word_count, word);
    if (count_ptr) {
        (*count_ptr)++;
    } else {
        int initial = 1;
        chmap_insert(word_count, word, initial);
    }
}

// Print results
chmap_for_each(word_count, it, {
    const char *word = *chmap_iter_key_ptr(it);
    int count = *chmap_iter_val_ptr(it);
    printf("%s: %d\n", word, count);
});

chmap_destroy(word_count);
```

### Binary Search Tree Map (cbstmap)

Ordered map using self-balancing AVL tree.

#### Features

- Self-balancing AVL tree (|height(left) - height(right)| ≤ 1)
- O(log n) insert, delete, and search
- In-order iteration (sorted by key)
- All operations are iterative (no recursion, stack-safe)
- Configurable signed/unsigned key comparison
- Custom comparison function support

#### Basic Operations

```c
// Creation
cbmap_construct(tree, int, char*);                    // Signed integer keys
cbmap_construct_unsigned(tree2, unsigned int, int);   // Unsigned integer keys

// With custom comparison
int my_compare(const void *a, const void *b) {
    return strcmp(*(char**)a, *(char**)b);
}
cbmap_construct_cc(string_tree, char*, int, my_compare);

// Insertion (always use variables - automatic rebalancing)
int key = 42;
char *value = "hello";
cbmap_insert(tree, key, value);

// Multiple insertions
int key2 = 10;
char *value2 = "world";
cbmap_insert(tree, key2, value2);

// Retrieval
int lookup_key = 42;
char *val = cbmap_get(tree, lookup_key);      // Returns value, fatal_err if not found
char **ptr = cbmap_get_ptr(tree, lookup_key); // Returns pointer or NULL

// Removal (automatic rebalancing)
int remove_key = 42;
ccol_retval_t result = cbmap_remove(tree, remove_key);

// Iteration - RECOMMENDED METHOD (traverses in sorted key order)
// The cbmap_for_each macro is the standard way to iterate
cbmap_for_each(tree, it, {
    int k = *cbmap_iter_key_ptr(it);
    char *v = *cbmap_iter_val_ptr(it);
    printf("Key: %d, Value: %s\n", k, v);
});

// Manual iteration (if you need more control)
// Again, please notice that this approach declares
// the iterator in the outer scope. The cbmap_for_each
// does not have that problem.
cbmap_iter_declare(tree, it);
for (it = cbmap_begin(tree); it != NULL; it = cbmap_iter_next(it)) {
    int k = *cbmap_iter_key_ptr(it);
    char *v = *cbmap_iter_val_ptr(it);
    printf("Key: %d, Value: %s\n", k, v);
}

// Cleanup
cbmap_reset(tree);    // Remove all nodes
cbmap_destroy(tree);  // Destroy and set to NULL
```

#### Example: Range Queries

```c
cbmap_construct(scores, int, char*);

// Insert student scores (use variables)
int score1 = 95;
char *name1 = "Alice";
cbmap_insert(scores, score1, name1);

int score2 = 82;
char *name2 = "Bob";
cbmap_insert(scores, score2, name2);

int score3 = 78;
char *name3 = "Charlie";
cbmap_insert(scores, score3, name3);

int score4 = 91;
char *name4 = "Diana";
cbmap_insert(scores, score4, name4);

// Iterate in sorted order (by score)
printf("Scores from lowest to highest:\n");
cbmap_for_each(scores, it, {
    int *score = cbmap_iter_key_ptr(it);
    char **name = cbmap_iter_val_ptr(it);
    printf("%s: %d\n", *name, *score);
});

cbmap_destroy(scores);
```

### Sorting (csort)

Generic stable sorting using iterative mergesort.

#### Features

- Stable sort (preserves relative order of equal elements)
- O(n log n) time complexity in all cases
- O(n) space complexity for temporary buffer
- Iterative implementation (no recursion, no stack overflow risk)
- Default comparison functions for all standard C types
- Works with any collection type via getter abstraction

#### Basic Operations

```c
// Sort a vector (most common and simplest use case)
cvec_construct(vec, int);
// ... add elements ...
cvec_sort(vec);                              // Default comparison
cvec_sort_with_comparison_proc(vec, my_cmp); // Custom comparison

// Sort a C array
int numbers[] = {5, 2, 8, 1, 9};
size_t count = sizeof(numbers) / sizeof(numbers[0]);

// Getter function for plain arrays
void *array_getter(void *collection, size_t index) {
    return &((int*)collection)[index];
}

// Custom comparison
int int_compare(const void *a, const void *b) {
    int x = *(const int*)a;
    int y = *(const int*)b;
    return (x > y) - (x < y);
}

// Sort the array
csort_sort(numbers, count, sizeof(int), array_getter, int_compare, NULL);
```

#### Example: Sorting Custom Structures

```c
typedef struct {
    char name[50];
    int age;
} Person;

int compare_by_age(const void *a, const void *b) {
    const Person *p1 = (const Person*)a;
    const Person *p2 = (const Person*)b;
    return (p1->age > p2->age) - (p1->age < p2->age);
}

void *person_array_getter(void *collection, size_t index) {
    return &((Person*)collection)[index];
}

Person people[] = {
    {"Alice", 30},
    {"Bob", 25},
    {"Charlie", 35}
};

csort_sort(people, 3, sizeof(Person), person_array_getter, compare_by_age, NULL);

for (int i = 0; i < 3; i++) {
    printf("%s: %d years old\n", people[i].name, people[i].age);
}
```

### Memory Pools (cmempool)

Fixed-size and ranged memory pool allocators for efficient memory management.

#### Features

- O(1) allocation and deallocation
- Thread-safe or single-threaded operation
- Preallocated buffer support (useful for embedded systems)
- Optional fallback to dynamic allocation
- Corruption detection via assertions
- Zero external fragmentation (fixed-size pools)

#### Fixed-Size Memory Pool

```c
// Create a pool of 100 elements, each 64 bytes
mempool *pool = mempool_create(100, 64, false, false, NULL, NULL);
//                               │   │    │      │      │     └─ error string
//                               │   │    │      │      └─ memory management
//                               │   │    │      └─ single threaded?
//                               │   │    └─ fallback to malloc?
//                               │   └─ element size
//                               └─ element count

// Allocate entries
void *entry1 = mempool_alloc_entry(pool);
void *entry2 = mempool_calloc_entry(pool);  // Zero-initialized

// Use the entries
strcpy(entry1, "Hello");

// Free entries back to pool
mempool_free_entry(entry1);
mempool_free_entry(entry2);

// Cleanup
mempool_destroy(pool);
```

#### Preallocated Buffer (Embedded Systems)

```c
// Declare a buffer in static storage
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(my_buffer, 100, 64);

// Create pool from preallocated buffer
mempool *pool = mempool_create_from_preallocated_buffer(
    my_buffer, sizeof(my_buffer), 64, false, true, NULL, NULL);

// Use normally...
void *entry = mempool_alloc_entry(pool);

// Cleanup (buffer itself is not freed)
mempool_destroy(pool);
```

#### Ranged Memory Pool

For allocations of varying sizes:

```c
// Create ranged pool: sizes from 2^4 (16) to 2^12 (4096) bytes
// With 2^9 (512) elements in the smallest pool
// -------------------------------
// | Elem size    |   Elem count |
// |------------------------------
// | 2^4  (16)    |   2^9 (512)  |
// | 2^5  (32)    |   2^8 (256)  |
// | 2^6  (64)    |   2^7 (128)  |
// | 2^7  (128)   |   2^6 (64)   |
// | 2^8  (256)   |   2^5 (32)   |
// | 2^9  (512)   |   2^4 (16)   |
// | 2^10 (1024)  |   2^3 (8)    |
// | 2^11 (2048)  |   2^2 (4)    |
// | 2^12 (4096)  |   2^1 (2)    |
// -------------------------------
r_mempool *rpool = r_mempool_create(4, 12, 9,
                                    // Try to allocate from the internal buffers first, when
                                    // they are exhausted, fallback to the provided memory
                                    // management mechanism (here it is heap, since the second
                                    // pointer from the end is NULL).
                                    fallback_at_last_exhaustion,
                                    false, // not-single-threaded, internal locks are enabled
                                    NULL,  // No memory management procs are provided, fallback to heap.
                                    NULL); // We are not interested in any error strings.

// Allocate various sizes (automatically selects appropriate sub-pool)
void *small = r_mempool_alloc_entry(rpool, 20);   // Uses 32-byte pool
void *medium = r_mempool_alloc_entry(rpool, 100); // Uses 128-byte pool
void *large = r_mempool_alloc_entry(rpool, 500);  // Uses 512-byte pool

// Reallocate if needed
medium = r_mempool_realloc_entry(rpool, medium, 200);  // Will move to 256-byte pool from 128-byte pool

// Query pool statistics
size_t used = r_mempool_used_count(rpool, 100);
size_t capacity = r_mempool_total_capacity(rpool, 100);
printf("Pool utilization: %zu/%zu\n", used, capacity);

// Cleanup
r_mempool_free_entry(small);
r_mempool_free_entry(medium);
r_mempool_free_entry(large);
r_mempool_destroy(rpool);
```

### Thread Communication (cthreadcomm)

Thread-safe message passing primitives.

#### Features

- Zero-copy semantics (ownership transfer)
- Designed to accept dynamically allocated data buffers
- Three abstractions: circular queue, dynamic queue, and bidirectional channel
- Blocking, non-blocking, and timed operations
- Thread-safe
- Message ownership transferr prevents data races

#### Circular Queue (Strictly Bounded)

Fixed-size queue with blocking backpressure:

```c
// Create queue that holds max 10 messages
circular_queue *cq = circular_queue_create(10, NULL);

// Send messages (blocks if full)
c_message_t msg = {
    .data = strdup("Hello"),
    .size = 6
};
circq_send_zc(cq, &msg);  // msg.data is now NULL (ownership transferred)

// Try to send without blocking
c_message_t msg2 = { .data = strdup("World"), .size = 6 };
ccol_retval_t result = circq_try_send_zc(cq, &msg2);
if (result == ccol_container_full) {
    printf("Queue full, message not sent\n");
    free(msg2.data);  // Still own the data if send failed
}

// Receive messages (blocks if empty)
c_message_t received;
circq_recv_zc(cq, &received);
printf("Received: %s\n", (char*)received.data);
free(received.data);  // Now we own and must free the data

// Cleanup
circular_queue_destroy(cq);
```

#### Dynamic Queue (Loosely Bounded)

Linked-list based queue that is allowed to grow dynamically up to **`max(size_t)-1`**
unconsumed messages, as long as there is enough memory:

```c
dynamic_queue *dq = dynamic_queue_create(NULL);

// Send never fails (unless memory exhausted) and never gets blocked
c_message_t msg = { .data = strdup("Message"), .size = 8 };
dynq_send_zc(dq, &msg);

// Receive works same as circular queue
c_message_t received;
dynq_recv_zc(dq, &received);
free(received.data);

dynamic_queue_destroy(dq);
```

#### Channel (Bidirectional, Strictly Bounded)

Owner-worker communication pattern:

```c
// Create channel with capacity 10 in each direction
channel *ch = channel_create(10, NULL);

// From worker thread: send to owner
void *worker_thread(void *arg) {
    channel *ch = (channel*)arg;
    
    c_message_t msg = { .data = strdup("Result"), .size = 7 };
    chan_send_zc(ch, &msg);  // Automatically routes to workers_to_owner queue
    
    return NULL;
}

// From owner thread: send to workers
c_message_t task = { .data = strdup("Task"), .size = 5 };
chan_send_zc(ch, &task);  // Automatically routes to owner_to_workers queue

// Receive from workers
c_message_t result;
chan_recv_zc(ch, &result);
printf("Worker result: %s\n", (char*)result.data);
free(result.data);

// Control flow
chan_disable_sending(ch, owner_to_workers);  // Stop new tasks
chan_enable_sending(ch, owner_to_workers);   // Resume tasks

// Query state
size_t pending = chan_msg_count(ch, workers_to_owner);
printf("%zu messages pending from workers\n", pending);

channel_destroy(ch);
```

#### Example: Producer-Consumer

```c
#include <cthreadcomm.h>
#include <pthread.h>

circular_queue *queue;

void *producer(void *arg) {
    for (int i = 0; i < 100; i++) {
        int *data = malloc(sizeof(int));
        *data = i;
        
        c_message_t msg = { .data = data, .size = sizeof(int) };
        circq_send_zc(queue, &msg);
    }
    
    // Send sentinel
    c_message_t sentinel = { .data = NULL, .size = 0 };
    circq_send_zc(queue, &sentinel);
    
    return NULL;
}

void *consumer(void *arg) {
    while (1) {
        c_message_t msg;
        circq_recv_zc(queue, &msg);
        
        if (msg.data == NULL) {
            break;  // Sentinel received
        }
        
        int value = *(int*)msg.data;
        printf("Consumed: %d\n", value);
        free(msg.data);
    }
    
    return NULL;
}

int main(void) {
    queue = circular_queue_create(10, NULL);
    
    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer, NULL);
    pthread_create(&cons, NULL, consumer, NULL);
    
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    
    circular_queue_destroy(queue);
    return 0;
}
```

## API Conventions

### Naming Patterns

The library tries to follow consistent naming conventions:

- **Container types**: `cvec`, `chmap`, `cbmap`, `mempool`, `r_mempool`, etc.
- **Creation**: `*_create()`, `*_create_full()`, `*_create_mp()`, etc.
- **Destruction**: `*_destroy()` (macro that nullifies pointer)
- **Operations**: `*_push()`, `*_pop()`, `*_insert()`, `*_get()`, `*_remove()`, etc.
- **Convenience macros**: `*_construct()`, `*_construct_scoped()`, `*_declare()`, `*_init()`
- **Iterators**: `*_for_each()`, `*_begin()`, `*_iter_next()`, etc.

### Internal Functions

Functions and macros prefixed with underscore(s) are internal and should not be called directly:

- `_mem_alloc()`, `_mem_free()` - Internal memory management
- `__cvector_destroy()`, `___cbmap_destroy()` - Internal destructors (use macros instead)
- `___csort_qsort()` - Internal sort implementation (use `csort_sort()` macro)
- `_populate_cmap_pair()` - Internal helper for maps

Always use the public macros and functions documented in this README.

### Iteration Patterns

The library provides `*_for_each` macros as the **recommended way to iterate** through key-value containers:

```c
// Hash maps - iteration order is undefined
chmap_for_each(map, it, {
    int key = *chmap_iter_key_ptr(it);
    char *val = *chmap_iter_val_ptr(it);
    // Process key and value
});

// BST maps - iteration is in sorted key order (in-order traversal)
cbmap_for_each(tree, it, {
    int key = *cbmap_iter_key_ptr(it);
    char *val = *cbmap_iter_val_ptr(it);
    // Process key and value in ascending key order
});

// Vectors - just use index-based iteration
for (size_t i = 0; i < cvec_size(vec); i++) {
    int val = cvec_at(vec, i);
    // Process value
}
```

**For maps**, the `*_for_each` macro:
- Automatically declares the iterator variable
- Handles iteration setup and advancement
- Provides cleaner, more readable code
- Uses a block `{ }` for the loop body

**Iterator macros** (`*_iter_key_ptr` and `*_iter_val_ptr`):
- Already have type information from the container declaration
- No need to pass type parameters
- Return properly-typed pointers automatically

If you need manual iteration control:

```c
chmap_iter_declare(map, it);
for (it = chmap_begin(map); it != NULL; it = chmap_iter_next(it)) {
    int *key = chmap_iter_key_ptr(it);
    // Manual control
    // ...
}
```

### Error Handling

Most functions return `ccol_retval_t`, if executed directly:

```c
ccol_retval_t result = chmap_remove(map, key);
switch (result) {
    case ccol_success:
        printf("Key removed\n");
        break;
    case ccol_key_not_found:
        printf("Key not found\n");
        break;
    default:
        printf("Error: %d\n", result);
}
```

Convenience macros typically call `fatal_err()` on failures
caused by the buggy caller code to make them noticed or when
there is a shortage of resources:

```c
chmap_insert(map, key, value);  // Terminates program on failure
```

For non-fatal error handling, one can use the underlying functions:

```c
cmap_pair key_pair = { .ptr = &key, .size = sizeof(key) };
cmap_pair val_pair = { .ptr = &value, .size = sizeof(value) };
ccol_retval_t result = chmap_insert_elem(map, &key_pair, &val_pair);
if (result != ccol_success) {
    // Handle error gracefully
}
```

## Thread Safety

### Containers Are Not Thread-Safe by Design

**Important:** Vector, hash map, and BST map implementations **do not include internal locks**. This is by design,
as locking only during the access operation (e.g., insert, get) would not provide a meaningful protection against
race conditions in typical usage patterns.

Consider this example:

```c
// Thread 1
if (chmap_get_ptr(map, key) == NULL) {
    // Race condition: Thread 2 might insert here
    chmap_insert(map, key, value);
}

// Thread 2
chmap_insert(map, key, other_value);
```

Even if individual operations were internally locked, the check-then-insert pattern remains racy. Proper thread safety requires higher-level synchronization:

```c
// Correct: Lock around the entire logical operation
pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

// Thread 1
pthread_mutex_lock(&map_lock);
if (chmap_get_ptr(map, key) == NULL) {
    chmap_insert(map, key, value);
}
pthread_mutex_unlock(&map_lock);
```

### Thread-Safe Components

The following components **are** thread-safe by default:

- **Memory pools** (`mempool`, `r_mempool`) - Unless created with `single_threaded=true`
- **Thread communication** (`circular_queue`, `dynamic_queue`, `channel`) - Always thread-safe

```c
// Thread-safe (not single-threaded) memory pool
mempool *pool = mempool_create(100, 64, false, false, NULL, NULL);
//                               multi-threaded ─┘

// Single-threaded pool (faster, not thread-safe)
mempool *fast_pool = mempool_create(100, 64, false, true, NULL, NULL);
//                                   single-threaded ─┘
```

### Recommended Patterns

For shared containers:

1. **Per-thread containers** (no synchronization needed)
2. **Reader-writer locks** for read-heavy workloads:
   ```c
   rw_lock_t lock = PTHREAD_RWLOCK_INITIALIZER;
   
   // Readers
   rw_lock_rdlock(lock);
   int val = chmap_get(map, key);
   rw_lock_unlock(lock);
   
   // Writers
   rw_lock_wrlock(lock);
   chmap_insert(map, key, value);
   rw_lock_unlock(lock);
   ```

3. **Mutexes** for simpler use cases:
   ```c
   mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   
   mutex_lock(lock);
   chmap_insert(map, key, value);
   mutex_unlock(lock);
   ```

## Memory Management

### Default Allocators

By default, all containers use standard `malloc`, `calloc`, `realloc`, and `free`.

### Custom Memory Management

All containers support custom memory management through `ccol_memmgmt_procs_t`:

```c
typedef struct {
    ccol_memmgmt_procs_alloc_t alloc;      // malloc equivalent
    ccol_memmgmt_procs_calloc_t calloc;    // calloc equivalent
    ccol_memmgmt_procs_realloc_t realloc;  // realloc equivalent
    ccol_memmgmt_procs_free_t free;        // free equivalent
} ccol_memmgmt_procs_t;
```

#### Example: Custom Allocator

```c
void *my_alloc(size_t size) {
    void *ptr = custom_malloc(size);
    printf("Allocated %zu bytes at %p\n", size, ptr);
    return ptr;
}

void *my_calloc(size_t count, size_t size) {
    void *ptr = custom_calloc(count, size);
    printf("Allocated %zu elements at %p\n", count, ptr);
    return ptr;
}

void *my_realloc(void *ptr, size_t size) {
    void *new_ptr = custom_realloc(ptr, size);
    printf("Reallocated %p to %zu bytes -> %p\n", ptr, size, new_ptr);
    return new_ptr;
}

void my_free(void *ptr) {
    printf("Freeing %p\n", ptr);
    custom_free(ptr);
}

// Setup custom allocators
ccol_memmgmt_procs_t my_mprocs = {
    .alloc = my_alloc,
    .calloc = my_calloc,
    .realloc = my_realloc,
    .free = my_free
};

// Use with any container
cvec_construct_mp(vec, int, &my_mprocs);
chmap_construct_mp(map, int, char*, 64, &my_mprocs);
mempool *pool = mempool_create(100, 64, false, false, &my_mprocs, NULL);
```

### Memory Pool Integration

Containers can use memory pools for their internal allocations:

```c
// Create a memory pool for map nodes
r_mempool *node_pool = r_mempool_create(4, 10, 6, 
                                        fallback_at_last_exhaustion,
                                        false, NULL, NULL);

// Wrapper functions
void *pool_alloc(size_t size) {
    return r_mempool_alloc_entry(node_pool, size);
}

void *pool_calloc(size_t count, size_t size) {
    return r_mempool_calloc_entry(node_pool, count * size);
}

void *pool_realloc(void *ptr, size_t size) {
    return r_mempool_realloc_entry(node_pool, ptr, size);
}

void pool_free(void *ptr) {
    r_mempool_free_entry(ptr);
}

ccol_memmgmt_procs_t pool_mprocs = {
    .alloc = pool_alloc,
    .calloc = pool_calloc,
    .realloc = pool_realloc,
    .free = pool_free
};

// Map now allocates from the pool
chmap_construct_mp(map, int, int, 64, &pool_mprocs);

// ... use map ...

chmap_destroy(map);
r_mempool_destroy(node_pool);
```

### Memory Ownership

#### Vectors

Vectors copy elements:

```c
cvec_construct(vec, int);
int x = 42;
cvec_push(vec, x);
x = 100;  // Original variable unchanged
printf("%d\n", cvec_at(vec, 0));  // Still 42
```

#### Maps

Maps copy keys and values:

```c
chmap_construct(map, int, int);
int key = 1, value = 100;
chmap_insert(map, key, value);
key = 2;    // Map's key still 1
value = 200; // Map's value still 100
```

For string data, the map copies the string content:

```c
chmap_construct(map, int, char*);

char *str = strdup("hello");
int key = 1;
chmap_insert(map, key, str);
free(str);  // This is FINE - map copied the string (SSO or allocation)

// When you retrieve strings from the map, you get pointers to the map's internal storage
int lookup = 1;
char *value = chmap_get(map, lookup);  // Points to map's copy
printf("%s\n", value);  // OK to use
// Don't free 'value' - it belongs to the map

// Map cleanup automatically frees all internal string storage
chmap_destroy(map);
```

For non-string pointer types, you need to manage the pointed-to data yourself:

```c
typedef struct { int x, y; } Point;
chmap_construct(points, int, Point*);

Point *p = malloc(sizeof(Point));
p->x = 10; p->y = 20;
int key = 1;
chmap_insert(points, key, p);
free(p);  // DON'T DO THIS - map stores the pointer value, not a copy of the Point
// Here, since the map was constructed as (int -> Point*), it will store
// the pointers as values, not the areas pointed by those pointers. Therefore
// managing the lifecycles of the pointers is the responsibility of the caller.
// Please notice that this code will be faster, since the pointers are some
// unsigned long variables to contain addresses.

// Correct cleanup:
chmap_for_each(points, it, {
    Point *ptr = *chmap_iter_val_ptr(it);
    free(ptr);  // Free the pointed-to data
});
chmap_destroy(points);  // Then destroy the map
```

#### Thread Communication

Messages use zero-copy transfer:

```c
c_message_t msg = { .data = malloc(100), .size = 100 };
circq_send_zc(queue, &msg);
// msg.data is now NULL - ownership transferred to queue

c_message_t received;
circq_recv_zc(queue, &received);
// received.data contains the original pointer - we now own it
free(received.data);
```

## License

MIT License

Copyright (c) 2024 A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
