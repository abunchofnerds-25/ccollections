# libccollections Documentation

**Version:** 1.0  
**License:** MIT License  
**Copyright:** 2024 A bunch of nerds

---

## Table of Contents

1. [Overview](#overview)
2. [Features](#features)
3. [Building and Installation](#building-and-installation)
4. [Core Components](#core-components)
   - [Common Infrastructure](#common-infrastructure)
   - [Vectors (cvector)](#vectors-cvector)
   - [Hash Maps (chashmap)](#hash-maps-chashmap)
   - [BST Maps (cbstmap)](#bst-maps-cbstmap)
   - [Thread Communication (cthreadcomm)](#thread-communication-cthreadcomm)
   - [Memory Pools (cmempool)](#memory-pools-cmempool)
   - [Sorting (csort)](#sorting-csort)
   - [Memory Operations (memops)](#memory-operations-memops)
5. [Usage Examples](#usage-examples)
6. [API Reference](#api-reference)
7. [Best Practices](#best-practices)

---

## Overview

**libccollections** is a comprehensive C library providing generic, high-performance data structures and algorithms. It offers a complete suite of collections, thread-safe communication primitives, memory management utilities, and sorting algorithms designed for systems programming.

### Design Philosophy

- **Type Safety**: Extensive use of C11 `_Generic` macros for compile-time type checking
- **Zero-Copy Semantics**: Message passing uses ownership transfer, not data copying
- **Customizable Memory Management**: All containers support custom allocators
- **Thread Safety**: Opt-in thread-safety with minimal overhead
- **Performance**: Optimized implementations with O(1) operations where possible
- **Embedded Systems**: Support for preallocated buffers and no-heap operation

### Key Technologies

- **C11 Standard**: Requires C11 compiler (GCC/Clang)
- **POSIX Threads**: Thread-safe operations use pthreads
- **GNU Extensions**: Statement expressions and compound literals
- **Iterative Algorithms**: Stack-safe, no recursion

---

## Features

### Data Structures

| Structure | Type | Complexity | Thread-Safe | Key Feature |
|-----------|------|------------|-------------|-------------|
| `cvector` | Dynamic Array | O(1) amortized | Optional | Auto-resizing vector |
| `chashmap` | Dual Hash Table | O(1) average | Optional | Open addressing + separate chaining |
| `cbstmap` | AVL Tree | O(log n) | Optional | Self-balancing BST |

### Thread Communication

| Primitive | Type | Blocking | Capacity | Use Case |
|-----------|------|----------|----------|----------|
| `circular_queue` | Bounded | Yes | Fixed | Backpressure control |
| `dynamic_queue` | Unbounded | Receive only | Unlimited* | Fire-and-forget |
| `channel` | Bidirectional | Configurable | Fixed | Worker threads |

*Limited by `max_elem_count (18446744073709551614)`

### Memory Management

| Component | Type | Allocation | Thread-Safe | Key Feature |
|-----------|------|------------|-------------|-------------|
| `mempool` | Fixed-size | O(1) | Optional | Free list, fallback |
| `r_mempool` | Ranged | O(1) | Optional | Multiple pools, power-of-2 |

### Algorithms

- **Sorting**: Iterative merge sort with custom comparators
- **Memory Operations**: Optimized small-copy routines (1-8 bytes)

---

## Building and Installation

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install build-essential gcc

# Fedora/RHEL
sudo dnf install gcc make
```

### Build Instructions

```bash
# Clone or extract the library
cd libccollections

# Build the shared library
make

# Run tests
make test

# Run with Valgrind
make memtest

# Generate coverage report
make generate_coverage_report

# Clean build artifacts
make clean
```

### Build Output

- `libccollections.so` - Shared library
- `obj/*.o` - Object files
- `main` - Test executable

### Compiler Flags

```makefile
CFLAGS = -I$(INCLUDE_DIR) -fstack-protector-all \
    -Wstrict-overflow -Wformat=2 -Wformat-security -Wall -Wextra \
    -g3 -O3 -Werror
LFLAGS = -shared -lpthread
```

### Linking

```bash
# Compile with library
gcc -L. -Iinclude myapp.c -o myapp -lccollections -lm -lpthread

# Run with library path
LD_LIBRARY_PATH=. ./myapp
```

---

## Core Components

## Common Infrastructure

**Header:** `common.h`

Provides foundational infrastructure used across all collection types.

### Threading Primitives

Thin wrappers around POSIX pthread primitives:

```c
mutex_t mutex;
mutex_init(mutex);
mutex_lock(mutex);
mutex_unlock(mutex);
mutex_destroy(mutex);

rw_lock_t rwlock;
rw_lock_init(&rwlock);
rw_lock_rdlock(&rwlock);  // Shared access
rw_lock_wrlock(&rwlock);  // Exclusive access
rw_lock_unlock(&rwlock);

cond_var_t cond;
cond_var_init(cond);
cond_var_wait(cond, mutex);
cond_var_signal(cond);
cond_var_broadcast(cond);
```

### Memory Management Abstraction

All containers support custom memory management:

```c
typedef struct ccol_memmgmt_procs_t {
  ccol_memmgmt_procs_alloc_t alloc;      // malloc-like
  ccol_memmgmt_procs_calloc_t calloc;    // calloc-like
  ccol_memmgmt_procs_realloc_t realloc;  // realloc-like
  ccol_memmgmt_procs_free_t free;        // free-like
} ccol_memmgmt_procs_t;
```

**Default allocators:**
```c
#define mem_alloc(size) malloc(size)
#define mem_calloc(elem_count, elem_size) calloc(elem_count, elem_size)
#define mem_realloc(ptr, new_size) realloc(ptr, new_size)
#define mem_free(ptr) free(ptr)
```

### Return Values

```c
typedef enum ccol_retval_t {
  ccol_success = 0,
  ccol_invalid_args,
  ccol_container_full,
  ccol_container_empty,
  ccol_not_enough_memory,
  ccol_key_not_found,
  ccol_not_permitted,
  ccol_timed_out,
  ccol_unexpected_failure
} ccol_retval_t;
```

### Error Handling

```c
// Error string with file:line
char *err = CCOL_ERR_STR("allocation failed");

// Fatal error (prints and asserts)
fatal_err("failed with code: %d", error_code);
```

### Map Key-Value Pairs

```c
typedef struct cmap_pair {
  void *ptr;    // Pointer to data
  size_t size;  // Size of data
} cmap_pair;

// Auto-populate based on type
cmap_pair kp, vp;
int key = 42;
char value[] = "hello";
_populate_cmap_pair(&kp, key);     // kp.ptr = &key, kp.size = 4
_populate_cmap_pair(&vp, value);   // vp.ptr = value, vp.size = 6
```

### Type Introspection

```c
// Data type enumeration
typedef enum ccollections_data_type {
  ccol_char, ccol_short, ccol_int, ccol_long, ccol_long_long,
  ccol_unsigned_char, ccol_unsigned_short, ccol_unsigned_int,
  ccol_unsigned_long, ccol_unsigned_long_long,
  ccol_float, ccol_double, ccol_long_double,
  ccol_pointer, ccol_string, ccol_other_types
} ccol_data_type;

// Determine type at compile time
ccol_data_type type = determine_ccol_data_type(my_variable);
```

---

## Vectors (cvector)

**Header:** `cvector.h`  
**Source:** `cvector.c`

Dynamic array with automatic resizing.

### Key Characteristics

- **Minimum capacity**: 4 elements
- **Scaling factor**: 2x growth, 0.5x shrinkage
- **Growth threshold**: When full
- **Shrink threshold**: When < 1/4 full
- **Complexity**: O(1) amortized push/pop
- **Memory**: Contiguous allocation

### Basic Operations

```c
// Create vector
char *err = NULL;
cvec vec = cvector_create(sizeof(int), &err);
if (!vec) {
    fprintf(stderr, "Error: %s\n", err);
    return;
}

// Push elements
int x = 42;
if (cvector_push_back(vec, &x) != ccol_success) {
    // Handle error
}

// Pop elements
int y;
if (cvector_pop_back(vec, &y) == ccol_success) {
    printf("Popped: %d\n", y);
}

// Access elements
int *ptr = (int*)cvector_at(vec, 0);
if (ptr) {
    printf("First: %d\n", *ptr);
}

// Size
size_t count = cvector_elem_count(vec);

// Clear
cvector_reset(vec);

// Destroy
cvector_destroy(vec);
```

### Type-Safe Macros

There are two ways to create a vector:

**Option 1: Declare then Init (two steps)**
```c
cvec_declare(vec, int);  // Declare with type
cvec_init(vec);          // Initialize
```

**Option 2: Construct (single step - combines both)**
```c
cvec_construct(vec, int);  // Declare + init in one

// Push/pop with automatic addressing
cvec_push(vec, x);              // Takes address automatically
cvec_push_rvalue(vec, 42);      // Can push literals
int val = cvec_pop(vec);        // Returns value

// Access
int val = cvec_at(vec, 0);      // Returns value
int *ptr = cvec_at_ptr(vec, 0); // Returns pointer
*ptr = 100;                     // Modify in place

// Size and reset
size_t size = cvec_size(vec);
cvec_reset(vec);
cvec_destroy(vec);
```

### Sorting

```c
// Sort with default comparison
cvec_construct(vec, int);
cvec_push_rvalue(vec, 3);
cvec_push_rvalue(vec, 1);
cvec_push_rvalue(vec, 2);
cvec_sort(vec);  // Now [1, 2, 3]

// Sort with custom comparison
int my_compare(const void *a, const void *b) {
    return *(int*)b - *(int*)a;  // Descending
}
cvector_sort_with_comparison_proc(vec, my_compare);
```

### Custom Memory Management

```c
ccol_memmgmt_procs_t mprocs = {
    .alloc = my_alloc,
    .calloc = my_calloc,
    .realloc = my_realloc,
    .free = my_free
};
cvec vec = cvector_create_with_mprocs(sizeof(int), &mprocs, &err);
```

---

## Hash Maps (chashmap)

**Header:** `chashmap.h`  
**Source:** `chashmap.c`

Hash table with **dual implementation strategy** for optimal performance.

### Key Characteristics

**Dual Implementation:**
- **Open Addressing** (linear probing) for integral types (int, float, double, etc.)
  - Compact 17-byte slot structure
  - Linear probing with prefetching
  - 70% max load factor, 25% min load factor
  - Fibonacci hashing for integers
- **Separate Chaining** (linked lists) for other types (strings, pointers, structs)
  - Doubly-linked list for insertion-order iteration
  - Small String Optimization (SSO) - up to 23 bytes inline
  - 1.5× load factor for scale-up, 1/8× for scale-down
  
**Common Features:**
- **Minimum buckets**: 64 (always power-of-2)
- **Scaling factor**: 4× up, 0.25× down
- **Default hash**: XXHash64 (not DJB2!)
- **Complexity**: O(1) average, O(n) worst case per bucket

### Hash Functions

**XXHash64** - Primary hash function for non-integral types:
```c
// Fast, high-quality 64-bit hash
uint64_t xxhash64(const void *data, size_t len, uint64_t seed)
```

**Fibonacci Hashing** - Optimized for integral types:
```c
// Multiplicative hash: key * 11400714819323198485ULL
size_t hash = key * FIBONACCI_HASH_MULTIPLIER;
```

### Basic Operations

```c
// Create map (automatically selects implementation based on key type)
char *err = NULL;
chmap map = chmap_create(64, ccol_int, &err);  // Uses open addressing
chmap str_map = chmap_create(64, ccol_string, &err);  // Uses separate chaining

// Insert key-value
cmap_pair kp, vp;
int key = 42;
char *value = "hello";
_populate_cmap_pair(&kp, key);
_populate_cmap_pair(&vp, value);

if (chmap_insert_elem(map, &kp, &vp) != ccol_success) {
    // Handle error
}

// Get value (copy)
char buffer[256];
if (chmap_get_elem_copy(map, &kp, buffer, sizeof(buffer)) == ccol_success) {
    printf("Value: %s\n", buffer);
}

// Get value (reference)
cmap_pair *val_ref;
if (chmap_get_elem_ref(map, &kp, &val_ref) == ccol_success) {
    printf("Value: %s\n", (char*)val_ref->ptr);
    // Don't free val_ref - it's owned by the map
}

// Delete
chmap_delete_elem(map, &kp);

// Count
size_t count = chmap_elem_count(map);

// Reset
chmap_reset(map, 0);  // Keep current size
chmap_reset(map, 128); // Resize to 128 buckets

// Destroy
chmap_destroy(map);
```

### Type-Safe Macros

There are two ways to create a hash map:

**Option 1: Declare then Init (two steps)**
```c
chmap_declare(map, int, char*);  // Declare with types
chmap_init(map);                 // Initialize
```

**Option 2: Construct (single step - combines both)**
```c
chmap_construct(map, int, char*);  // Declare + init in one
```

**Using the map:**
```c
// Insert
int key = 42;
char *value = "hello";
chmap_insert(map, key, value);

// Get (returns pointer to value)
char **val_ptr = chmap_get(map, key);
if (val_ptr) {
    printf("Value: %s\n", *val_ptr);
}

// Delete
chmap_delete(map, key);

// Iteration (insertion order for separate chaining)
for (chmap_iter_declare(map, it) = chmap_begin(map);
     it; it = chmap_iter_next(it)) {
    int *k = chmap_iter_key_ptr(it);
    char **v = chmap_iter_val_ptr(it);
    printf("%d -> %s\n", *k, *v);
}
```

### Custom Hash Function

```c
size_t my_hash(const void *key_data) {
    const int *k = (const int*)key_data;
    return *k * 2654435761u;  // Knuth's multiplicative hash
}

chmap map = chmap_create_ch(64, ccol_int, my_hash, &err);
```

### Small String Optimization (SSO)

For separate chaining maps, strings up to 23 bytes are stored inline:

```c
typedef struct chmap_entry {
  union {
    void* ptr;           // Heap-allocated for > 23 bytes
    char inline_data[24]; // Stack storage for ≤ 23 bytes
  } key_storage;
  bool key_is_inline;
  // ... same for value
} chmap_entry;
```

### Implementation Selection

The implementation is automatically chosen based on key type:

```c
// Integral types → Open Addressing
chmap_construct(int_map, int, char*);        // Open addressing
chmap_construct(float_map, float, int);      // Open addressing

// Other types → Separate Chaining
chmap_construct(str_map, char*, int);        // Separate chaining
chmap_construct(ptr_map, void*, char*);      // Separate chaining
```

---

## BST Maps (cbstmap)

**Header:** `cbstmap.h`  
**Source:** `cbstmap.c`

Self-balancing binary search tree (AVL tree) with sorted keys.

### Key Characteristics

- **Balance**: AVL tree (|height(left) - height(right)| ≤ 1)
- **Rotations**: Single (L/R) and double (LR/RL)
- **Implementation**: Iterative with explicit stacks (no recursion)
- **Iteration**: In-order traversal (sorted key order)
- **Complexity**: O(log n) all operations
- **Default comparison**: memcmp (unsigned), proper signed comparison

### AVL Balancing

The tree maintains balance through four rotation types:

```
Left-Left (LL):         Right-Right (RR):
    z                        z
   /                          \
  y        =>       y           y       =>      y
 /                /   \          \               / \
x                x     z          x             z   x

Left-Right (LR):        Right-Left (RL):
    z                        z
   /                          \
  x        =>       y           x       =>      y
   \               / \         /               / \
    y             x   z       y               z   x
```

### Basic Operations

```c
// Create map
char *err = NULL;
cbmap map = cbmap_create(false, &err);  // false = unsigned keys

// Insert (auto-balancing)
cmap_pair kp, vp;
int key = 42;
char *value = "hello";
_populate_cmap_pair(&kp, key);
_populate_cmap_pair(&vp, value);

cbmap_insert_elem(map, &kp, &vp);

// Get value
char buffer[256];
if (cbmap_get_elem_copy(map, &kp, buffer, sizeof(buffer)) == ccol_success) {
    printf("Value: %s\n", buffer);
}

// Delete (auto-rebalancing)
cbmap_delete_elem(map, &kp);

// Count
size_t count = cbmap_elem_count(map);

// Reset
cbmap_reset(map);

// Destroy
cbmap_destroy(map);
```

### Type-Safe Macros

There are two ways to create a BST map:

**Option 1: Declare then Init (two steps)**
```c
cbmap_declare(map, int, char*);  // Declare with types
cbmap_init(map);                 // Initialize (auto-detects signedness)
```

**Option 2: Construct (single step - combines both)**
```c
cbmap_construct(map, int, char*);  // Declare + init in one (auto-detects signedness)
```

**Using the map:**
```c
// Insert
cbmap_insert(map, key, value);

// Get
char **val = cbmap_get(map, key);

// Delete
cbmap_delete(map, key);

// Iteration (sorted order)
for (cbmap_iter_declare(map, it) = cbmap_begin(map);
     it; it = cbmap_iter_next(it)) {
    int *k = cbmap_iter_key_ptr(it);
    char **v = cbmap_iter_val_ptr(it);
    printf("%d -> %s\n", *k, *v);
}
```

**Custom initialization variants:**
```c
// Declare first
cbmap_declare(map, int, char*);

// Then init with custom options
cbmap_init_mp(map, &my_mem_procs);           // Custom memory management
cbmap_init_cc(map, my_comparison_func);      // Custom comparison
cbmap_init_full(map, &my_mem_procs, my_cmp); // Both custom
```

### Deletion Strategy

For nodes with two children:
- **Right deeper or equal**: Replace with right subtree minimum
- **Left deeper**: Replace with left subtree maximum

---

## Thread Communication (cthreadcomm)

**Header:** `cthreadcomm.h`  
**Source:** `cthreadcomm.c`

Thread-safe message passing primitives using zero-copy semantics.

### Zero-Copy Semantics

```c
typedef struct c_message_t {
  void *data;   // Ownership transfers on send
  size_t size;
} c_message_t;
```

**Key principle**: After successful send, sender's `data` pointer is set to NULL. Receiver gains ownership and must free.

### Circular Queue

**Bounded, blocking queue with backpressure.**

```c
// Create
circular_queue *cq = circular_queue_create(100, &err);

// Send (blocking)
c_message_t msg = {
    .data = malloc(256),
    .size = 256
};
strcpy(msg.data, "Hello");
circq_send_zc(cq, &msg);  // Blocks if full, msg.data becomes NULL

// Try send (non-blocking)
if (circq_try_send_zc(cq, &msg) == ccol_container_full) {
    // Queue is full
}

// Timed send
struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
ccol_retval_t ret = circq_timed_send_zc(cq, &msg, &timeout);

// Receive (blocking)
c_message_t recv_msg;
circq_recv_zc(cq, &recv_msg);
printf("%s\n", (char*)recv_msg.data);
free(recv_msg.data);  // Receiver must free

// Disable/enable sending
circq_disable_sending(cq);  // Wakes all blocked senders
circq_enable_sending(cq);

// Count
size_t count = circq_msg_count(cq);

// Destroy
circular_queue_destroy(cq);
```

### Dynamic Queue

**Unbounded queue, send never blocks.**

```c
// Create
dynamic_queue *dq = dynamic_queue_create(&err);

// Send (never blocks)
c_message_t msg = {.data = malloc(256), .size = 256};
dynq_send_zc(dq, &msg);

// Receive (blocking)
c_message_t recv_msg;
dynq_recv_zc(dq, &recv_msg);
free(recv_msg.data);

// Try receive (non-blocking)
if (dynq_try_recv_zc(dq, &recv_msg) == ccol_container_empty) {
    // Queue is empty
}

// Timed receive
struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
dynq_timed_recv_zc(dq, &recv_msg, &timeout);

// Count
size_t count = dynq_msg_count(dq);

// Destroy
dynamic_queue_destroy(dq);
```

### Channel

**Bidirectional owner-worker communication.**

```c
// Create
channel *ch = channel_create(100, &err);  // 100 = queue capacity

// Owner sends to worker
c_message_t msg = {.data = malloc(256), .size = 256};
channel_send_to_worker_zc(ch, &msg);

// Worker receives from owner
c_message_t recv_msg;
channel_recv_from_owner_zc(ch, &recv_msg);
// Process...
free(recv_msg.data);

// Worker sends response to owner
c_message_t resp = {.data = malloc(128), .size = 128};
channel_send_to_owner_zc(ch, &resp);

// Owner receives response
channel_recv_from_worker_zc(ch, &recv_msg);
free(recv_msg.data);

// Control
channel_disable_sending_to_worker(ch);
channel_enable_sending_to_worker(ch);

// Count
size_t to_worker = channel_to_worker_msg_count(ch);
size_t to_owner = channel_to_owner_msg_count(ch);

// Destroy
channel_destroy(ch);
```

### Producer-Consumer Example

```c
void *producer(void *arg) {
    circular_queue *cq = (circular_queue*)arg;
    
    for (int i = 0; i < 100; i++) {
        c_message_t msg = {
            .data = malloc(sizeof(int)),
            .size = sizeof(int)
        };
        *(int*)msg.data = i;
        circq_send_zc(cq, &msg);
    }
    
    // Send sentinel
    c_message_t sentinel = {.data = NULL, .size = 0};
    circq_send_zc(cq, &sentinel);
    return NULL;
}

void *consumer(void *arg) {
    circular_queue *cq = (circular_queue*)arg;
    
    while (1) {
        c_message_t msg;
        circq_recv_zc(cq, &msg);
        
        if (msg.data == NULL) {
            break;  // Sentinel received
        }
        
        printf("Received: %d\n", *(int*)msg.data);
        free(msg.data);
    }
    return NULL;
}

// Usage
circular_queue *cq = circular_queue_create(10, NULL);
pthread_t prod_thread, cons_thread;
pthread_create(&prod_thread, NULL, producer, cq);
pthread_create(&cons_thread, NULL, consumer, cq);
pthread_join(prod_thread, NULL);
pthread_join(cons_thread, NULL);
circular_queue_destroy(cq);
```

---

## Memory Pools (cmempool)

**Header:** `cmempool.h`  
**Source:** `cmempool.c`

Fixed-size and ranged memory allocators.

### Fixed-Size Memory Pool

**O(1) allocation/deallocation with free list.**

```c
// Create pool
mempool *mp = mempool_create(
    100,              // 100 elements
    64,               // 64 bytes each
    true,             // Fallback to dynamic when exhausted
    false,            // Thread-safe
    NULL,             // Default allocator
    &err
);

// Allocate
void *ptr = mempool_alloc_entry(mp);
if (ptr) {
    // Use memory...
}

// Allocate zeroed
void *zptr = mempool_calloc_entry(mp);

// Free
mempool_free_entry(ptr);  // Sets ptr to NULL

// Query
size_t total = mempool_total_capacity(mp);
size_t used = mempool_used_count(mp);
size_t dynamic = mempool_dynamic_allocs_count(mp);

// Destroy
mempool_destroy(mp);
```

### Preallocated Buffer Pool

**For embedded systems or avoiding heap.**

```c
// Declare buffer
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(buffer, 100, 64);

// Create pool from buffer
mempool *mp = mempool_create_from_preallocated_buffer(
    buffer,
    sizeof(buffer),
    64,
    false,            // No fallback
    true,             // Single-threaded
    NULL,
    &err
);

// Use normally...
void *ptr = mempool_alloc_entry(mp);
mempool_free_entry(ptr);

// Destroy (buffer is NOT freed)
mempool_destroy(mp);
```

### Ranged Memory Pool

**Multiple pools for power-of-2 sizes.**

```c
// Create ranged pool
// Pool 0: 2^4 = 16 bytes, 2^8 = 256 elements
// Pool 1: 2^5 = 32 bytes, 2^7 = 128 elements
// Pool 2: 2^6 = 64 bytes, 2^6 = 64 elements
r_mempool *rmp = r_mempool_create(
    4,                              // Smallest: 2^4 = 16 bytes
    6,                              // Largest: 2^6 = 64 bytes
    8,                              // Smallest count: 2^8 = 256
    fallback_at_first_exhaustion,   // Fallback policy
    false,                          // Thread-safe
    NULL,
    &err
);

// Allocate (automatically selects pool)
void *ptr16 = r_mempool_alloc_entry(rmp, 12);   // Uses 16-byte pool
void *ptr32 = r_mempool_alloc_entry(rmp, 20);   // Uses 32-byte pool
void *ptr64 = r_mempool_alloc_entry(rmp, 50);   // Uses 64-byte pool

// Allocate zeroed
void *zptr = r_mempool_calloc_entry(rmp, 16);

// Free (automatically returns to correct pool)
r_mempool_free_entry(ptr16);
r_mempool_free_entry(ptr32);

// Query
size_t pool_count = r_mempool_get_pool_count(rmp);
for (size_t i = 0; i < pool_count; i++) {
    size_t elem_size = r_mempool_get_ith_pool_elem_size(rmp, i);
    size_t capacity = r_mempool_get_ith_pool_capacity(rmp, i);
    size_t used = r_mempool_get_ith_pool_used_count(rmp, i);
    printf("Pool %zu: %zu bytes, %zu/%zu used\n", 
           i, elem_size, used, capacity);
}

// Destroy
r_mempool_destroy(rmp);
```

### Preallocated Ranged Pool

```c
// Declare buffer
DECLARE_PREALLOCATED_RANGED_MEMPOOL_BUFFER(buffer, 4, 6, 8);

r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
    buffer,
    sizeof(buffer),
    4, 6, 8,
    fallback_disabled,
    true,
    NULL,
    &err
);

// Use and destroy as normal
```

### Fallback Policies

```c
typedef enum r_memory_fallback_policy_t {
  fallback_disabled,              // Never use heap
  fallback_at_first_exhaustion,   // Each pool has fallback
  fallback_at_last_exhaustion,    // Only after all pools exhausted
} r_memory_fallback_policy_t;
```

### Corruption Detection

Memory pools detect corruption via:
- **Magic values**: Entry status markers
- **Back-pointers**: Each entry points to owning pool
- **Double-free detection**: Status checks
- **Assertions**: Extensive validation

```c
#define MEMPOOL_ENTRY_TAKEN (size_t)0xDEADBEEFDEADBEEF
#define MEMPOOL_ENTRY_FREE (size_t)0xFEEDBEADFEEDBEAD
#define NOT_A_POOL_MEMBER (size_t)0xBADC0FFEEBADC0FE
```

---

## Sorting (csort)

**Header:** `csort.h`  
**Source:** `csort.c`

Generic sorting with iterative merge sort.

### Algorithm

**Iterative bottom-up merge sort:**
- Start with subarrays of size 1
- Merge pairs: 1→2, 2→4, 4→8, ...
- No recursion, stack-safe
- Stable sort
- O(n log n) time, O(n) space

### Default Comparisons

Automatic selection via `_Generic`:

```c
// Signed integers
csort_default_char_comparison_proc
csort_default_short_comparison_proc
csort_default_int_comparison_proc
csort_default_long_comparison_proc
csort_default_long_long_comparison_proc

// Unsigned integers
csort_default_unsigned_char_comparison_proc
csort_default_unsigned_short_comparison_proc
csort_default_unsigned_int_comparison_proc
csort_default_unsigned_long_comparison_proc
csort_default_unsigned_long_long_comparison_proc

// Floating-point
csort_default_float_comparison_proc
csort_default_double_comparison_proc
csort_default_long_double_comparison_proc

// Strings
csort_default_string_comparison_proc
```

### Array Sorting

```c
// Array getter
void *array_getter(void *collection, size_t index) {
    int *arr = (int*)collection;
    return &arr[index];
}

// Sort array
int arr[] = {3, 1, 4, 1, 5, 9, 2, 6};
size_t len = sizeof(arr) / sizeof(arr[0]);

// With default comparison
int dummy;
csort_sort(arr, len, sizeof(int), array_getter,
           csort_get_default_comparison_proc(dummy), NULL, NULL);

// With custom comparison (descending)
int cmp_desc(const void *a, const void *b) {
    return *(int*)b - *(int*)a;
}
csort_sort(arr, len, sizeof(int), array_getter, cmp_desc, NULL, NULL);
```

### Vector Sorting

```c
cvec_construct(vec, int);
// ... populate vec ...

// Default comparison (ascending)
cvec_sort(vec);

// Custom comparison
int cmp_desc(const void *a, const void *b) {
    return *(int*)b - *(int*)a;
}
cvector_sort_with_comparison_proc(vec, cmp_desc);
```

### Custom Collection Sorting

```c
// Define getter for your collection
void *my_getter(void *collection, size_t index) {
    my_collection_t *col = (my_collection_t*)collection;
    return my_collection_get(col, index);
}

// Define comparison
int my_compare(const void *a, const void *b) {
    const my_type_t *x = (const my_type_t*)a;
    const my_type_t *y = (const my_type_t*)b;
    // Return -1, 0, or 1
}

// Sort
csort_sort(my_col, count, sizeof(my_type_t),
           my_getter, my_compare, NULL, NULL);
```

---

## Memory Operations (memops)

**Header:** `memops.h`  
**Source:** `memops.c`

Optimized memory operations for small sizes.

### Small Copy Optimization

```c
void mem_cpy(void* dst, const void* src, size_t n);
```

**Optimization for sizes 0-8:**
- **Direct assignments** using packed structs
- **Eliminates loop overhead** for common sizes
- **Falls back to memcpy** for larger sizes

```c
// Size-specific optimizations
switch (n) {
  case 0: return;
  case 1: *(uint8_t*)dst = *(uint8_t*)src; return;
  case 2: *(uint16_t*)dst = *(uint16_t*)src; return;
  case 4: *(uint32_t*)dst = *(uint32_t*)src; return;
  case 8: *(uint64_t*)dst = *(uint64_t*)src; return;
  // 3, 5, 6, 7 use packed structs
}
```

### Small Zero Optimization

```c
void mem_zero(void* dst, size_t n);
```

**Similar optimization for zeroing:**

```c
// Size-specific zeroing
switch (n) {
  case 0: return;
  case 1: *(uint8_t*)dst = 0; return;
  case 2: *(uint16_t*)dst = 0; return;
  case 4: *(uint32_t*)dst = 0; return;
  case 8: *(uint64_t*)dst = 0; return;
  // 3, 5, 6, 7 use packed structs
}
```

### Packed Structures

```c
typedef struct s3 {
  uint16_t _0_;
  uint8_t _1_;
} __attribute__((packed)) s3;

typedef struct s5 {
  uint32_t _0_;
  uint8_t _1_;
} __attribute__((packed)) s5;

typedef struct s6 {
  uint32_t _0_;
  uint16_t _1_;
} __attribute__((packed)) s6;

typedef struct s7 {
  uint32_t _0_;
  uint16_t _1_;
  uint8_t _2_;
} __attribute__((packed)) s7;
```

### Usage

```c
// Automatically optimized for common element sizes
cvector uses mem_cpy for element copying
circular_queue uses mem_cpy for message copying
Memory pools use mem_zero for calloc operations
```

---

## Usage Examples

### Example 1: Simple Vector

```c
#include <cvector.h>
#include <stdio.h>

int main(void) {
    // Option 1: construct (single step)
    cvec_construct(vec, int);
    
    // Option 2: declare + init (two steps)
    // cvec_declare(vec, int);
    // cvec_init(vec);
    
    // Add elements
    for (int i = 0; i < 10; i++) {
        cvec_push_rvalue(vec, i * i);
    }
    
    // Print
    for (size_t i = 0; i < cvec_size(vec); i++) {
        printf("%d ", cvec_at(vec, i));
    }
    printf("\n");
    
    // Sort
    cvec_sort(vec);
    
    // Pop
    while (cvec_size(vec) > 0) {
        printf("%d ", cvec_pop(vec));
    }
    printf("\n");
    
    cvec_destroy(vec);
    return 0;
}
```

### Example 2: Hash Map with Strings

```c
#include <chashmap.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // Option 1: construct (single step)
    chmap_construct(map, int, char*);
    
    // Option 2: declare + init (two steps)
    // chmap_declare(map, int, char*);
    // chmap_init(map);
    
    // Insert
    chmap_insert(map, 1, "one");
    chmap_insert(map, 2, "two");
    chmap_insert(map, 3, "three");
    
    // Get
    char **val = chmap_get(map, 2);
    if (val) {
        printf("Key 2: %s\n", *val);
    }
    
    // Iterate (insertion order for separate chaining maps)
    for (chmap_iter_declare(map, it) = chmap_begin(map);
         it; it = chmap_iter_next(it)) {
        int *k = chmap_iter_key_ptr(it);
        char **v = chmap_iter_val_ptr(it);
        printf("%d -> %s\n", *k, *v);
    }
    
    chmap_destroy(map);
    return 0;
}
```

### Example 3: BST Map - Sorted Iteration

```c
#include <cbstmap.h>
#include <stdio.h>

int main(void) {
    // Option 1: construct (single step)
    cbmap_construct(map, unsigned int, double);
    
    // Option 2: declare + init (two steps)
    // cbmap_declare(map, unsigned int, double);
    // cbmap_init(map);
    
    // Insert in random order
    cbmap_insert(map, 50, 5.0);
    cbmap_insert(map, 30, 3.0);
    cbmap_insert(map, 70, 7.0);
    cbmap_insert(map, 20, 2.0);
    cbmap_insert(map, 40, 4.0);
    
    // Iterate in sorted key order
    printf("Sorted iteration:\n");
    for (cbmap_iter_declare(map, it) = cbmap_begin(map);
         it; it = cbmap_iter_next(it)) {
        unsigned int *k = cbmap_iter_key_ptr(it);
        double *v = cbmap_iter_val_ptr(it);
        printf("%u -> %.1f\n", *k, *v);
    }
    
    cbmap_destroy(map);
    return 0;
}
```

### Example 4: Producer-Consumer with Queue

```c
#include <cthreadcomm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int id;
    char message[256];
} task_t;

void *producer(void *arg) {
    circular_queue *cq = arg;
    
    for (int i = 0; i < 5; i++) {
        task_t *task = malloc(sizeof(task_t));
        task->id = i;
        snprintf(task->message, sizeof(task->message), 
                 "Task %d", i);
        
        c_message_t msg = {
            .data = task,
            .size = sizeof(task_t)
        };
        circq_send_zc(cq, &msg);
        printf("Produced: Task %d\n", i);
    }
    
    // Sentinel
    c_message_t sentinel = {NULL, 0};
    circq_send_zc(cq, &sentinel);
    return NULL;
}

void *consumer(void *arg) {
    circular_queue *cq = arg;
    
    while (1) {
        c_message_t msg;
        circq_recv_zc(cq, &msg);
        
        if (msg.data == NULL) break;
        
        task_t *task = msg.data;
        printf("Consumed: %s\n", task->message);
        free(task);
    }
    return NULL;
}

int main(void) {
    circular_queue *cq = circular_queue_create(10, NULL);
    
    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer, cq);
    pthread_create(&cons, NULL, consumer, cq);
    
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    
    circular_queue_destroy(cq);
    return 0;
}
```

### Example 5: Memory Pool Allocation

```c
#include <cmempool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int id;
    char name[64];
    double value;
} record_t;

int main(void) {
    // Create pool for 1000 records
    mempool *mp = mempool_create(1000, sizeof(record_t),
                                 true, false, NULL, NULL);
    
    // Allocate records
    record_t *records[100];
    for (int i = 0; i < 100; i++) {
        records[i] = mempool_calloc_entry(mp);
        records[i]->id = i;
        snprintf(records[i]->name, 64, "Record %d", i);
        records[i]->value = i * 3.14;
    }
    
    // Use records...
    printf("Record 50: %s, %.2f\n", 
           records[50]->name, records[50]->value);
    
    // Free records
    for (int i = 0; i < 100; i++) {
        mempool_free_entry(records[i]);
    }
    
    // Stats
    printf("Pool capacity: %zu\n", mempool_total_capacity(mp));
    printf("Pool used: %zu\n", mempool_used_count(mp));
    printf("Dynamic allocs: %zu\n", mempool_dynamic_allocs_count(mp));
    
    mempool_destroy(mp);
    return 0;
}
```

### Example 6: Ranged Memory Pool

```c
#include <cmempool.h>
#include <stdio.h>

int main(void) {
    // Pools: 16, 32, 64, 128 bytes
    r_mempool *rmp = r_mempool_create(
        4,  // 2^4 = 16 bytes min
        7,  // 2^7 = 128 bytes max
        8,  // 2^8 = 256 elements in smallest pool
        fallback_at_first_exhaustion,
        false, NULL, NULL
    );
    
    // Allocate various sizes (automatically routed)
    void *p1 = r_mempool_alloc_entry(rmp, 10);   // → 16-byte pool
    void *p2 = r_mempool_alloc_entry(rmp, 20);   // → 32-byte pool
    void *p3 = r_mempool_alloc_entry(rmp, 50);   // → 64-byte pool
    void *p4 = r_mempool_alloc_entry(rmp, 100);  // → 128-byte pool
    
    // Print pool stats
    size_t pool_count = r_mempool_get_pool_count(rmp);
    for (size_t i = 0; i < pool_count; i++) {
        printf("Pool %zu: %zu bytes, %zu/%zu used\n",
               i,
               r_mempool_get_ith_pool_elem_size(rmp, i),
               r_mempool_get_ith_pool_used_count(rmp, i),
               r_mempool_get_ith_pool_capacity(rmp, i));
    }
    
    // Free
    r_mempool_free_entry(p1);
    r_mempool_free_entry(p2);
    r_mempool_free_entry(p3);
    r_mempool_free_entry(p4);
    
    r_mempool_destroy(rmp);
    return 0;
}
```

---

## API Reference

### Return Codes

```c
typedef enum ccol_retval_t {
  ccol_success = 0,              // Operation successful
  ccol_invalid_args,             // NULL pointer or invalid argument
  ccol_container_full,           // Container at capacity
  ccol_container_empty,          // Container has no elements
  ccol_not_enough_memory,        // Allocation failed
  ccol_key_not_found,            // Key does not exist in map
  ccol_not_permitted,            // Operation not allowed (e.g., sending disabled)
  ccol_timed_out,                // Timeout expired
  ccol_unexpected_failure        // System error (check errno)
} ccol_retval_t;
```

### Memory Management

```c
// Function pointer types
typedef void *(*ccol_memmgmt_procs_alloc_t)(size_t size);
typedef void *(*ccol_memmgmt_procs_calloc_t)(size_t elem_count, size_t elem_size);
typedef void *(*ccol_memmgmt_procs_realloc_t)(void *ptr, size_t new_size);
typedef void (*ccol_memmgmt_procs_free_t)(void *ptr);

// Procedure structure
typedef struct ccol_memmgmt_procs_t {
  ccol_memmgmt_procs_alloc_t alloc;
  ccol_memmgmt_procs_calloc_t calloc;
  ccol_memmgmt_procs_realloc_t realloc;
  ccol_memmgmt_procs_free_t free;
} ccol_memmgmt_procs_t;
```

### Comparison and Hashing

```c
// Comparison function type (-1, 0, 1)
typedef int (*ccol_comparison_proc_t)(const void *first, const void *second);

// Hashing function type
typedef size_t (*ccol_hashing_proc_t)(const void *key_data);
```

### Iterator

```c
typedef struct cmap_iterator {
  cmap_pair *key_pair;    // Pointer to key
  cmap_pair *val_pair;    // Pointer to value
  // Internal fields...
} cmap_iterator;
```

---

## Best Practices

### Memory Management

1. **Always check return values**
   ```c
   if (cvector_push_back(vec, &elem) != ccol_success) {
       // Handle error
   }
   ```

2. **Use type-safe macros**
   ```c
   cvec_construct(vec, int);  // Preferred
   // vs
   cvec vec = cvector_create(sizeof(int), &err);  // Manual
   ```

3. **Custom allocators for pools**
   ```c
   mempool *mp = mempool_create(...);
   ccol_memmgmt_procs_t mprocs = {
       .alloc = (ccol_memmgmt_procs_alloc_t)mempool_alloc_entry,
       .free = (ccol_memmgmt_procs_free_t)_mempool_free_entry,
       // ...
   };
   cvec vec = cvector_create_with_mprocs(sizeof(int), &mprocs, &err);
   ```

### Thread Safety

1. **Choose appropriate queue type**
   - **Circular queue**: Bounded, backpressure, predictable memory
   - **Dynamic queue**: Unbounded, no backpressure, unpredictable memory
   - **Channel**: Bidirectional, worker pattern

2. **Enable thread-safety when needed**
   ```c
   mempool *mp = mempool_create(100, 64, false, false, NULL, &err);
   //                                           ^^^^^ thread-safe
   ```

3. **Use sentinels for shutdown**
   ```c
   c_message_t sentinel = {.data = NULL, .size = 0};
   circq_send_zc(queue, &sentinel);
   ```

### Performance

1. **Preallocate for known sizes**
   ```c
   // For vectors, use push operations instead of frequent resets
   cvec vec = cvector_create(sizeof(large_struct), &err);
   // ... use ...
   cvector_reset(vec);  // Reuse instead of destroy/create
   ```

2. **Use memory pools for frequent allocations**
   ```c
   // Instead of malloc/free in loop
   mempool *mp = mempool_create(1000, sizeof(node_t), true, false, NULL, NULL);
   for (...) {
       node_t *node = mempool_alloc_entry(mp);
       // ...
       mempool_free_entry(node);
   }
   ```

3. **Choose correct map type**
   - **Hash map**: Fast average case, unordered
   - **BST map**: Slower, ordered iteration, range queries possible

### Error Handling

1. **Check error strings**
   ```c
   char *err = NULL;
   cvec vec = cvector_create(sizeof(int), &err);
   if (!vec) {
       fprintf(stderr, "Error: %s\n", err);
       return -1;
   }
   ```

2. **Use fatal_err for unrecoverable errors**
   ```c
   if (critical_failure) {
       fatal_err("Critical: %s", reason);  // Prints and asserts
   }
   ```

### Memory Pools

1. **Size pools appropriately**
   ```c
   // Calculate expected peak usage
   size_t peak_concurrent = 100;
   size_t safety_factor = 1.5;
   mempool *mp = mempool_create(
       peak_concurrent * safety_factor,
       elem_size,
       true,  // Fallback for bursts
       false, NULL, NULL
   );
   ```

2. **Monitor dynamic allocations**
   ```c
   size_t dynamic = mempool_dynamic_allocs_count(mp);
   if (dynamic > threshold) {
       // Consider increasing pool size
   }
   ```

3. **Use ranged pools for varied sizes**
   ```c
   // Instead of multiple fixed pools
   r_mempool *rmp = r_mempool_create(4, 10, 8, ...);
   ```

### Iterators

1. **Don't modify during iteration**
   ```c
   // BAD
   for (chmap_iter_declare(map, it) = chmap_begin(map); it; it = chmap_iter_next(it)) {
       chmap_insert(map, new_key, new_val);  // Invalidates iterator!
   }
   
   // GOOD
   cvec_declare(to_insert);
   cvec_construct(to_insert, pair_t);
   for (chmap_iter_declare(map, it) = chmap_begin(map); it; it = chmap_iter_next(it)) {
       // Collect items to insert
       cvec_push(to_insert, ...);
   }
   // Insert after iteration
   for (size_t i = 0; i < cvec_size(to_insert); i++) {
       chmap_insert(map, ...);
   }
   ```

2. **Use automatic cleanup**
   ```c
   // Hash map iterator auto-destroys
   for (chmap_iter_declare(map, it) = chmap_begin(map); 
        it; it = chmap_iter_next(it)) {
       // No manual cleanup needed
   }
   
   // BST map iterator needs manual cleanup
   cbmap_iter_declare(map, it);
   for (it = cbmap_begin(map); it; it = cbmap_iter_next(it)) {
       // Auto-destroys at end
   }
   ```

### Sorting

1. **Use default comparisons when possible**
   ```c
   cvec_sort(vec);  // Automatic type-based comparison
   ```

2. **Stable sort for secondary keys**
   ```c
   // Merge sort is stable
   cvec_sort_by_secondary_key(vec);
   cvec_sort_by_primary_key(vec);
   // Elements with equal primary keys maintain secondary order
   ```

---

## Troubleshooting

### Common Issues

**Issue:** Segmentation fault on iterator
```c
// BAD: Iterator invalidated by modification
for (chmap_iter_declare(map, it) = chmap_begin(map); it; it = chmap_iter_next(it)) {
    chmap_delete(map, *chmap_iter_key_ptr(it));  // CRASH!
}

// GOOD: Collect keys first
cvec_declare(keys);
cvec_construct(keys, int);
for (chmap_iter_declare(map, it) = chmap_begin(map); it; it = chmap_iter_next(it)) {
    cvec_push(keys, *chmap_iter_key_ptr(it));
}
for (size_t i = 0; i < cvec_size(keys); i++) {
    chmap_delete(map, cvec_at(keys, i));
}
```

**Issue:** Memory leak in queue
```c
// BAD: Messages not freed
while (circq_try_recv_zc(queue, &msg) == ccol_success) {
    // Process msg.data but don't free
}

// GOOD: Always free received data
c_message_t msg;
while (circq_try_recv_zc(queue, &msg) == ccol_success) {
    // Process msg.data
    free(msg.data);  // Receiver must free
}
```

**Issue:** Pool corruption assertion
```c
// BAD: Double free
void *ptr = mempool_alloc_entry(mp);
mempool_free_entry(ptr);
mempool_free_entry(ptr);  // ASSERT!

// GOOD: Macro sets pointer to NULL
void *ptr = mempool_alloc_entry(mp);
mempool_free_entry(ptr);  // ptr is now NULL
mempool_free_entry(ptr);  // Safe no-op
```

### Debug Build

```bash
# Build with debug symbols and assertions
make CFLAGS="-g3 -O0 -DDEBUG"

# Run in gdb
make run-in-gdb

# Run with valgrind
make run-in-valgrind
```

---

## License

MIT License - See source files for full text.

## Contributors

A bunch of nerds

## Version History

- **1.0** (2024): Initial release
  - Vectors, hash maps, BST maps
  - Thread communication primitives
  - Memory pools
  - Sorting algorithms
  - Memory operation optimizations

---

**End of Documentation**