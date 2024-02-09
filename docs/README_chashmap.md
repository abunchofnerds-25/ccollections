# Hash Map Library (chashmap)

High-performance hash map with automatic resizing and O(1) average-case operations.

## Overview

`chashmap.h` provides a hash table implementation with separate chaining using linked lists for collision resolution. Features automatic dynamic resizing, custom hashing functions, and type-safe macros for convenient usage.

## Key Features

- **O(1) Average Operations** - Insert, lookup, and delete
- **Automatic Resizing** - Grows 4x and shrinks 0.25x based on load factor
- **Separate Chaining with Linked Lists** - Simple and efficient collision resolution
- **DJB2 Hash Function** - Fast default hashing with optional custom functions
- **Insertion Order Iteration** - Doubly-linked list maintains insertion order
- **Type-Safe Macros** - Compile-time type checking with GNU C extensions
- **Custom Memory Management** - Optional custom allocators

## Quick Start

### Basic Usage

```c
#include <chashmap.h>

// Create hash map
chmap_construct(users, int, char*);  // key: int, value: char*

// Insert key-value pairs
chmap_insert(users, 1, "Alice");
chmap_insert(users, 2, "Bob");
chmap_insert(users, 3, "Charlie");

// Lookup
char *name = chmap_get(users, 2);
printf("User 2: %s\n", name);  // "Bob"

// Update (upsert semantics)
chmap_insert(users, 2, "Robert");

// Delete
chmap_remove(users, 1);

// Check existence
char *name_ptr = chmap_get_ptr(users, 4);
if (name_ptr) {
    printf("Found: %s\n", *name_ptr);
} else {
    printf("Not found\n");
}

// Iterate in insertion order
for (chmap_iter_declare(users, it) = chmap_begin(users);
     it; it = chmap_iter_next(it)) {
    printf("Key: %d, Value: %s\n",
           *(int*)it->key_pair->ptr,
           *(char**)it->val_pair->ptr);
}

// Cleanup (automatic with chmap_construct)
chmap_destroy(users);
```

### String Keys

```c
chmap_construct(config, char*, char*);

chmap_insert(config, "host", "localhost");
chmap_insert(config, "port", "8080");
chmap_insert(config, "timeout", "30");

char *host = chmap_get(config, "host");
printf("Connecting to %s\n", host);

chmap_destroy(config);
```

### Struct Values

```c
typedef struct {
    char name[32];
    int age;
    double salary;
} Employee;

chmap_construct(employees, int, Employee);

Employee e1 = {"Alice", 30, 75000.0};
chmap_insert(employees, 101, e1);

Employee emp = chmap_get(employees, 101);
printf("%s: %d years, $%.2f\n", emp.name, emp.age, emp.salary);

// Modify in-place
Employee *emp_ptr = chmap_get_ptr(employees, 101);
if (emp_ptr) {
    emp_ptr->salary = 80000.0;
}

chmap_destroy(employees);
```

## API Overview

### Creation & Destruction

```c
// Declare and construct (with automatic cleanup)
chmap_construct(map_name, key_type, value_type);

// With custom memory management
chmap_construct_mp(map_name, key_type, value_type, mmgmt_procs);

// With custom hashing
chmap_construct_ch(map_name, key_type, value_type, custom_hash_func);

// Full customization
chmap_construct_full(map_name, key_type, value_type, mmgmt_procs, custom_hash_func);

// Manual init (after chmap_declare)
chmap_init(map_name);
chmap_init_mp(map_name, mmgmt_procs);
chmap_init_ch(map_name, custom_hash_func);

// Destroy
chmap_destroy(map_name);
```

### Operations

```c
// Insert or update (upsert)
chmap_insert(map_name, key, value);

// Lookup (terminates on not found)
value_type val = chmap_get(map_name, key);

// Lookup (returns NULL on not found)
value_type *ptr = chmap_get_ptr(map_name, key);

// Remove
ccol_retval_t result = chmap_remove(map_name, key);

// Get element count
size_t count = chmap_elem_count(map_name);

// Clear all elements
ccol_retval_t result = chmap_reset(map_name, 0);

// Clear and resize
ccol_retval_t result = chmap_reset(map_name, new_size);
```

### Iteration

```c
// Iterate in insertion order
for (chmap_iter_declare(map_name, it) = chmap_begin(map_name);
     it; it = chmap_iter_next(it)) {
    // Access key and value
    const key_type *key = (const key_type*)it->key_pair->ptr;
    value_type *val = (value_type*)it->val_pair->ptr;
}

// Manual iterator management
cmap_iterator *it = chmap_begin(map_name);
while (it) {
    // Process entry
    it = chmap_iter_next(it);
}
```

### Low-Level API

```c
// Create with specific bucket size
chmap map = chmap_create(initial_bucket_size, &err);
chmap map = chmap_create_mp(initial_bucket_size, mmgmt_procs, &err);
chmap map = chmap_create_ch(initial_bucket_size, custom_hash_func, &err);
chmap map = chmap_create_full(initial_bucket_size, mmgmt_procs, custom_hash_func, &err);

// Insert with cmap_pair
ccol_retval_t chmap_insert_elem(chmap chm, const cmap_pair *key_pair,
                                 const cmap_pair *val_pair);

// Get copy
ccol_retval_t chmap_get_elem_copy(chmap chm, const cmap_pair *key_pair,
                                   void *target_buf, size_t target_buf_size);

// Get reference
ccol_retval_t chmap_get_elem_ref(chmap chm, const cmap_pair *key_pair,
                                  cmap_pair **val_pair);

// Delete
ccol_retval_t chmap_delete_elem(chmap chm, const cmap_pair *key_pair);
```

## Performance Characteristics

### Time Complexity

| Operation | Average | Worst Case | Notes |
|-----------|---------|------------|-------|
| Insert | O(1) | O(n) | Amortized, worst case per bucket where n is chain length |
| Lookup | O(1) | O(n) | Average with good hash function, n is chain length |
| Delete | O(1) | O(n) | Amortized, worst case per bucket where n is chain length |
| Iteration | O(n) | O(n) | Linear in number of elements |
| Resize | O(n) | O(n) | Triggered by load factor |

### Space Complexity

- **Storage**: O(n) where n is number of elements
- **Overhead**: ~32-40 bytes per element (entry + pointers)
- **Buckets**: Between n/4 and n*1.5 (due to resizing)

### Bucket Array Sizing

```
Minimum size: 63 (64-1, always power-of-2 minus 1)
Growth: 4x (e.g., 63 → 255 → 1023 → 4095)
Shrink: 0.25x (e.g., 4095 → 1023 → 255 → 63)
Scale up threshold: elem_count >= (bucket_count + 1) * 1.5
Scale down threshold: elem_count < (bucket_count + 1) / 8
```

## Hash Function

### Default (DJB2)
```c
unsigned long hash = 5381;
for (size_t i = 0; i < size; i++) {
    hash = ((hash << 5) + hash) + data[i];  // hash * 33 + c
}
```

### Custom Hashing
```c
unsigned long my_hash(const void *key) {
    int k = *(const int*)key;
    return (unsigned long)k * 2654435761UL;  // Knuth multiplicative
}

chmap_construct_ch(map, int, char*, my_hash);
```

### Hash Function Requirements

- **Deterministic**: Same input always produces same output
- **Uniform**: Distributes keys evenly across buckets
- **Fast**: Called on every insert/lookup/delete
- **Full Range**: Uses entire unsigned long range

## Collision Resolution

Uses separate chaining with linked lists:

```
Bucket Array:
┌─────┬─────┬─────┬─────┐
│  0  │  1  │  2  │  3  │
└──┬──┴──┬──┴──┬──┴──┬──┘
   │     │     │     │
   v     v     v     v
  List  List  List  List
   │
   v
  Node → Node → NULL
```

**Advantages:**
- Simple and efficient implementation
- Low memory overhead per collision
- Good average-case performance with proper hashing
- Fast insertion at head of chain

## Automatic Resizing

The map automatically grows and shrinks:

### Growth Example
```
Initial: 63 buckets, 0 elements
Insert 95 elements → triggers resize
New: 255 buckets (4x growth)
Threshold: (255+1) * 1.5 = 384 elements
```

### Shrink Example
```
Current: 255 buckets, 500 elements
Delete to 30 elements → triggers resize
Trigger: 30 < (255+1) / 8 = 32
New: 63 buckets (0.25x shrink)
```

### Resize Cost
- O(n) time to rehash all elements
- Amortized across many operations
- Can be avoided by pre-sizing appropriately

## Iteration Order

Unlike most hash maps, iteration maintains insertion order:

```c
chmap_construct(map, int, char*);

chmap_insert(map, 3, "third");
chmap_insert(map, 1, "first");
chmap_insert(map, 2, "second");

// Iterates in order: 3, 1, 2 (insertion order)
for (chmap_iter_declare(map, it) = chmap_begin(map);
     it; it = chmap_iter_next(it)) {
    // ...
}
```

Implementation: Doubly-linked list connects all entries independent of hash buckets.

## Type Safety

The type-safe macro layer prevents common errors:

```c
chmap_construct(int_to_str, int, char*);
chmap_construct(str_to_int, char*, int);

chmap_insert(int_to_str, 1, "one");     // OK
chmap_insert(int_to_str, "1", "one");   // Compile error: wrong key type
chmap_insert(str_to_int, "one", 1);     // OK
chmap_insert(str_to_int, 1, 1);         // Compile error: wrong key type

char *s = chmap_get(int_to_str, 1);     // OK: returns char*
int n = chmap_get(int_to_str, 1);       // Compile error: type mismatch
```

## Custom Memory Management

All allocations can use custom allocators:

```c
ccol_memmgmt_procs_t debug_allocator = {
    .malloc = debug_malloc,
    .calloc = debug_calloc,
    .realloc = debug_realloc,
    .free = debug_free
};

chmap_construct_mp(map, int, char*, &debug_allocator);
```

Useful for:
- Memory tracking and leak detection
- Arena allocators
- Garbage collection integration
- Embedded systems with custom memory managers

## Common Patterns

### Configuration Map
```c
chmap_construct(config, char*, char*);

void load_config(const char *filename) {
    FILE *f = fopen(filename, "r");
    char key[64], value[256];
    while (fscanf(f, "%s %s", key, value) == 2) {
        chmap_insert(config, key, value);
    }
    fclose(f);
}

const char *get_config(const char *key, const char *default_val) {
    char **val_ptr = chmap_get_ptr(config, key);
    return val_ptr ? *val_ptr : default_val;
}
```

### Frequency Counter
```c
chmap_construct(freq, char*, int);

void count_words(const char *text) {
    char *token = strtok(text, " ");
    while (token) {
        int *count = chmap_get_ptr(freq, token);
        if (count) {
            (*count)++;
        } else {
            chmap_insert(freq, token, 1);
        }
        token = strtok(NULL, " ");
    }
}
```

### Cache Implementation
```c
typedef struct {
    void *data;
    time_t timestamp;
} CacheEntry;

chmap_construct(cache, char*, CacheEntry);

void cache_put(const char *key, void *data) {
    CacheEntry entry = {data, time(NULL)};
    chmap_insert(cache, key, entry);
}

void *cache_get(const char *key) {
    CacheEntry *entry = chmap_get_ptr(cache, key);
    if (!entry) return NULL;
    
    // Check expiration (5 minute TTL)
    if (time(NULL) - entry->timestamp > 300) {
        chmap_remove(cache, key);
        return NULL;
    }
    
    return entry->data;
}
```

### Object Pool
```c
chmap_construct(objects, int, Object*);

Object *get_or_create(int id) {
    Object **obj_ptr = chmap_get_ptr(objects, id);
    if (obj_ptr) {
        return *obj_ptr;
    }
    
    Object *obj = create_object(id);
    chmap_insert(objects, id, obj);
    return obj;
}
```

## Thread Safety

**Maps are NOT thread-safe by default.** External synchronization required:

```c
chmap_construct(shared_map, int, int);
pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;

// Thread-safe read
pthread_rwlock_rdlock(&lock);
int *val = chmap_get_ptr(shared_map, key);
if (val) {
    result = *val;
}
pthread_rwlock_unlock(&lock);

// Thread-safe write
pthread_rwlock_wrlock(&lock);
chmap_insert(shared_map, key, value);
pthread_rwlock_unlock(&lock);
```

## Best Practices

1. **Pre-size for known capacity** - Avoids resize overhead
2. **Use chmap_get_ptr for existence checks** - Doesn't terminate
3. **Don't modify map during iteration** - Invalidates iterators
4. **Check return values** - Handle errors gracefully
5. **Use string interning for string keys** - Reduces memory
6. **Choose good hash functions** - Critical for performance
7. **Prefer chmap_construct** - Automatic cleanup

## Common Pitfalls

### ❌ Holding Pointers Across Modifications
```c
int *ptr = chmap_get_ptr(map, 1);
chmap_insert(map, 2, 42);  // May resize and rehash!
*ptr = 99;                 // DANGER: ptr may be invalid
```

### ✅ Correct Approach
```c
int *ptr = chmap_get_ptr(map, 1);
if (ptr) {
    *ptr = 99;  // Safe: no modifications
}
// Or copy the value
int val = chmap_get(map, 1);
chmap_insert(map, 2, 42);
val = 99;  // Safe: working with copy
```

### ❌ Modifying During Iteration
```c
for (chmap_iter_declare(map, it) = chmap_begin(map);
     it; it = chmap_iter_next(it)) {
    chmap_remove(map, key);  // DANGER: invalidates iterator
}
```

### ✅ Correct Approach
```c
// Collect keys to remove
cvec_construct(to_remove, int);
for (chmap_iter_declare(map, it) = chmap_begin(map);
     it; it = chmap_iter_next(it)) {
    int key = *(int*)it->key_pair->ptr;
    if (should_remove(key)) {
        cvec_push(to_remove, key);
    }
}
// Remove after iteration
for (size_t i = 0; i < cvec_size(to_remove); i++) {
    chmap_remove(map, cvec_at(to_remove, i));
}
```

## Memory Efficiency

### Overhead per Entry
```
Entry structure: ~40 bytes
- Key data: variable
- Value data: variable
- Hash value: 8 bytes
- Pointers: 24 bytes (doubly-linked list + next in bucket chain)
Total: ~40 bytes + key_size + value_size
```

### Pre-sizing Strategy
```c
// If you know you'll have ~1000 elements
chmap map = chmap_create(1024, NULL);  // Start with appropriate size
// Avoids multiple resize operations
```

## Performance Tuning

### Choose Appropriate Initial Size
```c
// Too small: many resizes
chmap map = chmap_create(64, NULL);    // Default

// Better for known size
chmap map = chmap_create(1024, NULL);  // If expecting ~1000 elements
```

### Optimize Hash Function
```c
// Poor hash: many collisions
unsigned long bad_hash(const void *p) {
    return *(int*)p % 100;  // Limited range
}

// Good hash: full range, uniform distribution
unsigned long good_hash(const void *p) {
    int k = *(int*)p;
    k = ((k >> 16) ^ k) * 0x45d9f3b;
    k = ((k >> 16) ^ k) * 0x45d9f3b;
    k = (k >> 16) ^ k;
    return k;
}
```

## Comparison with Other Maps

| Feature | chashmap | cbstmap | std::unordered_map |
|---------|----------|---------|-------------------|
| Lookup | O(1) avg | O(log n) | O(1) avg |
| Insertion Order | Yes | No | C++11+ only |
| Sorted Keys | No | Yes | No |
| Worst Case | O(n)* | O(log n) | O(n) |
| Memory Overhead | Low | Low | Medium |
| Resize Cost | O(n) | None | O(n) |

*Worst case O(n) where n is the collision chain length in a single bucket

## Integration

```c
// Include in your project
#include "chashmap.h"

// Compile
// gcc myprogram.c chashmap.c -o myprogram
```

## License

MIT License - See source file header for full license text.

## Related Components

- `common.h` - Common types and utilities