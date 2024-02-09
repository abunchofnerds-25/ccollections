# Common Utilities and Types (common)

Foundation library providing cross-cutting utilities, types, and abstractions used by all collection components.

## Overview

`common.h` serves as the foundation for the entire collections library, providing:
- Threading primitives (mutex, rwlock, condition variables)
- Error handling and reporting
- Memory management abstraction
- Type introspection via C11 _Generic
- Standard return codes
- Map key-value pair types

## Key Features

- **Portable Threading** - Wraps pthread with simple macros
- **Custom Memory Management** - Plugin architecture for custom allocators
- **Type Safety** - C11 _Generic for compile-time type checking
- **Error Reporting** - File:line error strings
- **Zero Dependencies** - Only standard C library and pthread

## Threading Primitives

### Mutex Operations

```c
#include <common.h>

mutex_t lock;
mutex_init(lock);

// Critical section
mutex_lock(lock);
// ... protected code ...
mutex_unlock(lock);

mutex_destroy(lock);
```

### Read-Write Lock

```c
rw_lock_t rwlock;
rw_lock_init(&rwlock);

// Multiple readers
void reader() {
    rw_lock_rdlock(&rwlock);
    // Read shared data
    rw_lock_unlock(&rwlock);
}

// Single writer
void writer() {
    rw_lock_wrlock(&rwlock);
    // Modify shared data
    rw_lock_unlock(&rwlock);
}

rw_lock_destroy(&rwlock);
```

### Condition Variables

```c
mutex_t lock;
cond_var_t cv;

mutex_init(lock);
cond_var_init(cv);

// Wait for condition
mutex_lock(lock);
while (!condition) {
    cond_var_wait(cv, lock);
}
mutex_unlock(lock);

// Signal condition
mutex_lock(lock);
condition = true;
cond_var_signal(cv);  // Wake one waiter
// or
cond_var_broadcast(cv);  // Wake all waiters
mutex_unlock(lock);

cond_var_destroy(cv);
mutex_destroy(lock);
```

### Timed Wait

```c
struct timespec timeout;
clock_gettime(CLOCK_REALTIME, &timeout);
timeout.tv_sec += 5;  // 5 second timeout

mutex_lock(lock);
int result = cond_var_timedwait(cv, lock, timeout);
if (result == ETIMEDOUT) {
    // Timeout occurred
}
mutex_unlock(lock);
```

### Thread IDs

```c
thread_id_t tid = get_thread_id();

if (pthread_equal(tid, other_tid)) {
    // Same thread
}
```

## Error Handling

### Error String Generation

```c
char *err = NULL;

// Function sets err on failure
mempool *mp = mempool_create(100, 256, false, false, NULL, &err);
if (!mp) {
    printf("Error: %s\n", err);
    // Output: "mempool.c:123 - failed to allocate buffer"
}
```

### Fatal Errors

```c
void critical_operation() {
    if (system_call() < 0) {
        fatal_err("System call failed: %s", strerror(errno));
        // Prints error and terminates via assert(false)
    }
}
```

### CCOL_ERR_STR Macro

```c
char *validate(int value) {
    if (value < 0) {
        return CCOL_ERR_STR("value must be non-negative");
        // Returns: "myfile.c:42 - value must be non-negative"
    }
    return NULL;
}
```

## Memory Management

### Default Allocators

```c
// Standard library wrappers
void *ptr = mem_alloc(1024);           // malloc(1024)
void *arr = mem_calloc(10, 100);       // calloc(10, 100)
ptr = mem_realloc(ptr, 2048);          // realloc(ptr, 2048)
mem_free(ptr);                         // free(ptr)
```

### Custom Memory Management

```c
// Define custom allocators
void *my_malloc(size_t size) {
    printf("Allocating %zu bytes\n", size);
    return malloc(size);
}

void my_free(void *ptr) {
    printf("Freeing %p\n", ptr);
    free(ptr);
}

ccol_memmgmt_procs_t custom_mm = {
    .malloc = my_malloc,
    .calloc = calloc,  // Can mix custom and standard
    .realloc = realloc,
    .free = my_free
};

// Use with collections
mempool *mp = mempool_create(100, 256, false, false, &custom_mm, NULL);
```

### Conditional Allocation

```c
// Use custom if provided, otherwise default
void *ptr = _mem_alloc(mprocs, 1024);
// Equivalent to:
// mprocs ? mprocs->malloc(1024) : malloc(1024)

void *arr = _mem_calloc(mprocs, 10, 100);
ptr = _mem_realloc(mprocs, ptr, 2048);
_mem_free(mprocs, ptr);
```

### Memory Management Validation

```c
ccol_memmgmt_procs_t incomplete = {
    .malloc = my_malloc,
    .free = my_free,
    .calloc = NULL,    // Missing!
    .realloc = NULL    // Missing!
};

char *err = NULL;
if (!ccol_verify_memmgmt_procs(&incomplete, &err)) {
    printf("Invalid: %s\n", err);
    // Output: "Detected at least one NULL memory management function"
}
```

## Return Value Codes

All collection operations return `ccol_retval_t`:

```c
typedef enum {
    ccol_unexpected_failure = -9,  // Unknown error
    ccol_container_empty,          // No elements
    ccol_container_full,           // At capacity
    ccol_timed_out,                // Operation timeout
    ccol_not_permitted,            // Invalid state
    ccol_invalid_args,             // Bad parameters
    ccol_key_not_found,            // Key doesn't exist
    ccol_key_already_present,      // Key exists (update)
    ccol_not_enough_memory,        // Allocation failed
    ccol_success = 0               // Success
} ccol_retval_t;
```

### Usage Example

```c
ccol_retval_t result = chmap_insert_elem(map, &key, &value);

switch (result) {
    case ccol_success:
        printf("Inserted successfully\n");
        break;
    case ccol_not_enough_memory:
        printf("Out of memory\n");
        break;
    case ccol_container_full:
        printf("Map at max capacity\n");
        break;
    default:
        printf("Error: %d\n", result);
}
```

## Type Introspection

### Checking for Numeric Types

```c
int x = 42;
double y = 3.14;
char *str = "hello";

if (is_integral_type(x)) {
    printf("x is a number\n");  // Prints
}

if (is_integral_type(y)) {
    printf("y is a number\n");  // Prints
}

if (is_integral_type(str)) {
    printf("str is a number\n");  // Doesn't print
}
```

### Checking for Pointers

```c
int *ptr = &x;
double *dptr = &y;

if (is_integral_ptr(ptr)) {
    printf("ptr points to a number\n");  // Prints
}
```

### Checking for Signed Integers

```c
int *signed_ptr = &x;
unsigned int *unsigned_ptr = &ux;

if (__is_signed_int_ptr(signed_ptr)) {
    printf("Signed integer pointer\n");  // Prints
}

if (__is_signed_int_ptr(unsigned_ptr)) {
    printf("Signed integer pointer\n");  // Doesn't print
}
```

### String Detection

```c
char str[] = "hello";
char *ptr = "world";
int num = 42;

if (is_char_ptr(str)) {
    printf("str is char pointer\n");  // Prints
}

if (is_char_ptr(ptr)) {
    printf("ptr is char pointer\n");  // Prints
}

if (is_char_array(str)) {
    printf("str is char array\n");  // Prints
}

if (is_char_array(ptr)) {
    printf("ptr is char array\n");  // Doesn't print (it's char*)
}
```

## Map Types

### Key-Value Pairs

```c
// Generic pair for any type
cmap_pair key_pair = {
    .ptr = &key_data,
    .size = sizeof(key_data)
};

cmap_pair val_pair = {
    .ptr = &val_data,
    .size = sizeof(val_data)
};

// Insert into map
chmap_insert_elem(map, &key_pair, &val_pair);
```

### Automatic Pair Population

```c
cmap_pair key_pair, val_pair;

int key = 42;
char *value = "hello";

// Automatically handles types and sizes
_populate_cmap_pair(&key_pair, key);
_populate_cmap_pair(&val_pair, value);

// For integers: ptr = &key, size = sizeof(int)
// For strings: ptr = value, size = strlen(value) + 1
```

### Map Iterators

```c
cmap_iterator *it = chmap_begin_iter(map, NULL);

while (it) {
    // Access key and value
    void *key = it->key_pair->ptr;
    size_t key_size = it->key_pair->size;
    
    void *value = it->val_pair->ptr;
    size_t val_size = it->val_pair->size;
    
    // Next element
    it = chmap_iter_next(it);
}
```

## Automatic Cleanup

### Destructor Attribute

```c
// Variable automatically cleaned up on scope exit
void example() {
    void cleanup_int(int **p) {
        free(*p);
        *p = NULL;
    }
    
    int *ptr _ccol_destructor(cleanup_int) = malloc(sizeof(int));
    *ptr = 42;
    
    // ptr automatically freed when leaving scope
}
```

### Collection Cleanup

```c
void example() {
    // Automatically destroyed on scope exit
    cvec_construct(numbers, int);
    cvec_push(numbers, 1);
    cvec_push(numbers, 2);
    
    // cvec_destroy(numbers) called automatically
}
```

## Custom Comparison and Hashing

### Comparison Functions

```c
int compare_ints(const void *a, const void *b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return (ia > ib) - (ia < ib);
}

// Use with BST map
cbmap map = cbmap_create_ch(false, compare_ints, NULL);
```

### Hashing Functions

```c
unsigned long hash_int(const void *ptr) {
    unsigned int val = *(const unsigned int*)ptr;
    val = ((val >> 16) ^ val) * 0x45d9f3b;
    val = ((val >> 16) ^ val) * 0x45d9f3b;
    val = (val >> 16) ^ val;
    return val;
}

// Use with hash map
chmap map = chmap_create_ch(64, hash_int, NULL);
```

## Constants and Limits

```c
// Maximum element count for collections
#define max_elem_count (SIZE_MAX - 1)

// Example usage
if (collection_size >= max_elem_count) {
    return ccol_container_full;
}
```

## Common Patterns

### Resource Management

```c
typedef struct {
    void *resource;
    ccol_memmgmt_procs_t *mprocs;
} ResourceHandle;

void cleanup_resource(ResourceHandle **handle) {
    if (*handle) {
        _mem_free((*handle)->mprocs, (*handle)->resource);
        _mem_free((*handle)->mprocs, *handle);
        *handle = NULL;
    }
}

void use_resource() {
    ResourceHandle *h _ccol_destructor(cleanup_resource) = 
        create_resource();
    
    // Use resource...
    
    // Automatically cleaned up
}
```

### Error Propagation

```c
ccol_retval_t process_data(void *data, char **err) {
    if (!data) {
        if (err) *err = CCOL_ERR_STR("data is NULL");
        return ccol_invalid_args;
    }
    
    void *buffer = mem_alloc(1024);
    if (!buffer) {
        if (err) *err = CCOL_ERR_STR("allocation failed");
        return ccol_not_enough_memory;
    }
    
    // Process...
    
    mem_free(buffer);
    return ccol_success;
}
```

### Custom Memory Pool

```c
typedef struct {
    void *pool;
    size_t used;
    size_t capacity;
} MemoryArena;

void *arena_alloc(size_t size) {
    // Allocate from arena
}

void arena_free(void *ptr) {
    // No-op for arena (freed all at once)
}

ccol_memmgmt_procs_t arena_mm = {
    .malloc = arena_alloc,
    .calloc = arena_calloc,
    .realloc = NULL,  // Not supported
    .free = arena_free
};
```

## Thread-Safe Collections

```c
typedef struct {
    chmap map;
    pthread_rwlock_t lock;
} ThreadSafeMap;

void ts_map_insert(ThreadSafeMap *tsm, int key, int value) {
    pthread_rwlock_wrlock(&tsm->lock);
    chmap_insert(tsm->map, key, value);
    pthread_rwlock_unlock(&tsm->lock);
}

int ts_map_get(ThreadSafeMap *tsm, int key, bool *found) {
    pthread_rwlock_rdlock(&tsm->lock);
    int *ptr = chmap_get_ptr(tsm->map, key);
    int result = ptr ? *ptr : 0;
    *found = (ptr != NULL);
    pthread_rwlock_unlock(&tsm->lock);
    return result;
}
```

## Compiler Compatibility

### Required Features
- C11 or later (for _Generic)
- GNU C extensions (for statement expressions)
- pthread support

### Supported Compilers
- GCC 4.9+
- Clang 3.4+
- ICC (with GNU compatibility mode)

### Preprocessor Differences

```c
#if defined __clang__
    // Clang-specific handling
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\"")
#else
    // GCC or other
#endif
```

## Best Practices

1. **Always check return values** - Even memory allocation can fail
2. **Use type-safe macros** - Prefer cvec_push over cvector_push_back
3. **Initialize error pointers** - Set `char *err = NULL` before passing
4. **Use custom allocators carefully** - All four functions must be provided
5. **Leverage automatic cleanup** - Use _ccol_destructor when possible
6. **Validate memory management** - Use ccol_verify_memmgmt_procs
7. **Handle threading explicitly** - Collections are not thread-safe by default

## Integration

```c
// Always include in your collection code
#include "common.h"

// Link with pthread
// gcc myprogram.c -pthread -o myprogram
```

## Platform Notes

### Linux
- Full support
- pthread typically included

### macOS
- Full support  
- pthread included

### Windows
- Requires pthread-win32 or similar
- Some modifications may be needed

### Embedded Systems
- Can use without pthread if single-threaded
- Define custom memory management for constrained environments

## Performance Considerations

### Memory Allocation Overhead

```c
// Direct allocation: ~100-500ns
void *p = malloc(1024);

// Through abstraction: +5-10ns
void *p = _mem_alloc(mprocs, 1024);
```

### Lock Overhead

```c
// Uncontended lock/unlock: ~20-50ns
mutex_lock(lock);
// ... critical section ...
mutex_unlock(lock);

// Read-write lock (read): ~30-60ns
// Read-write lock (write): ~40-70ns
```

## Debugging Tips

### Enable Assertions

```c
// Compile with assertions enabled
// gcc -DDEBUG myprogram.c

// Or disable for production
// gcc -DNDEBUG myprogram.c
```

### Memory Leak Detection

```c
ccol_memmgmt_procs_t debug_mm = {
    .malloc = debug_malloc_with_tracking,
    .calloc = debug_calloc_with_tracking,
    .realloc = debug_realloc_with_tracking,
    .free = debug_free_with_tracking
};

// Use debug_mm with all collections
// Check for leaks at program exit
```

### Thread Debugging

```c
// Log thread operations
#define DEBUG_THREADS

#ifdef DEBUG_THREADS
#define mutex_lock(m) do { \
    printf("[%lu] Locking %p\n", pthread_self(), &m); \
    pthread_mutex_lock(&m); \
} while(0)
#endif
```

## License

MIT License - See source file header for full license text.

## Related Components

All collection components depend on common.h:
- `cvector.h` - Dynamic vector
- `chashmap.h` - Hash map
- `cbstmap.h` - BST map
- `cmempool.h` - Memory pools
- `cthreadcomm.h` - Thread communication
- `csort.h` - Sorting library
