# Dynamic Vector Library (cvector)

High-performance dynamic array implementation with automatic resizing and type-safe macros.

## Overview

`cvector.h` provides a dynamic array (vector) container similar to C++ `std::vector` or Python's `list`. It automatically grows and shrinks as elements are added or removed, providing an efficient alternative to fixed-size arrays or manual reallocation.

## Key Features

- **Automatic Resizing** - Grows 2x when full, shrinks 0.5x when utilization drops
- **Type-Safe Macros** - GNU C extensions provide compile-time type checking
- **Optimized Operations** - Special handling for common sizes (1, 2, 4, 8 bytes)
- **Sorting Support** - Built-in integration with csort library
- **Custom Memory Management** - Optional custom allocators
- **Minimum Capacity** - Never shrinks below 4 elements

## Quick Start

### Basic Usage

```c
#include <cvector.h>

// Declare and construct a vector of integers
cvec_construct(numbers, int);

// Add elements
cvec_push(numbers, 42);
cvec_push(numbers, 17);
cvec_push(numbers, 99);

// Access elements
printf("First: %d\n", cvec_at(numbers, 0));
printf("Size: %zu\n", cvec_size(numbers));

// Iterate
for (size_t i = 0; i < cvec_size(numbers); i++) {
    printf("%d ", cvec_at(numbers, i));
}

// Cleanup (automatic if using cvec_construct)
cvec_destroy(numbers);
```

### Working with Strings

```c
// Vector of strings
cvec_construct(names, char*);

cvec_push(names, "Alice");
cvec_push(names, "Bob");
cvec_push(names, "Charlie");

for (size_t i = 0; i < cvec_size(names); i++) {
    printf("%s\n", cvec_at(names, i));
}

cvec_destroy(names);
```

### Working with Structs

```c
typedef struct {
    int id;
    char name[32];
    double score;
} Student;

cvec_construct(students, Student);

Student s1 = {1, "Alice", 95.5};
cvec_push(students, s1);

Student *ptr = cvec_at_ptr(students, 0);
ptr->score = 97.0;  // Modify in-place

cvec_destroy(students);
```

## API Overview

### Vector Creation & Destruction

```c
// Declare variable and type info
cvec_declare(vec_name, element_type);

// Initialize declared vector
cvec_init(vec_name);

// Declare + initialize in one step
cvec_construct(vec_name, element_type);

// Initialize with custom memory management
cvec_init_with_mprocs(vec_name, mmgmt_procs);

// Destroy vector and free memory
cvec_destroy(vec_name);
```

### Adding Elements

```c
// Add element to end (grows if needed)
cvec_push(vec_name, element);

// Add element and get pointer to it
element_type *ptr = cvec_push_get_ptr(vec_name, element);
```

### Accessing Elements

```c
// Get element by value (copy)
element_type val = cvec_at(vec_name, index);

// Get pointer to element (for in-place modification)
element_type *ptr = cvec_at_ptr(vec_name, index);

// Get last element
element_type last = cvec_at(vec_name, cvec_size(vec_name) - 1);
```

### Removing Elements

```c
// Remove and return last element
element_type val = cvec_pop(vec_name);

// Clear all elements (doesn't free capacity)
cvec_reset(vec_name);
```

### Querying

```c
// Get number of elements
size_t count = cvec_size(vec_name);

// Get current capacity
size_t cap = cvec_capacity(vec_name);

// Check if empty
if (cvec_size(vec_name) == 0) { /* empty */ }
```

### Sorting

```c
// Sort with default comparator (ascending)
cvec_sort(vec_name);

// Sort with custom comparator
int compare_desc(const void *a, const void *b) {
    return (*(int*)b) - (*(int*)a);
}
cvec_sort_with_comparator(vec_name, compare_desc);
```

### Low-Level Operations (Expert Use)

```c
// Direct access to underlying functions (no type safety)
cvector_push_back(vec, &element, sizeof(element));
cvector_at(vec, index, &result, sizeof(result));
void *ptr = cvector_at_ptr(vec, index);
```

## Performance Characteristics

### Time Complexity

| Operation | Average | Worst Case | Notes |
|-----------|---------|------------|-------|
| Push | O(1) | O(n) | Amortized O(1), occasional realloc |
| Pop | O(1) | O(n) | Amortized O(1), occasional shrink |
| Access (at) | O(1) | O(1) | Direct array indexing |
| Size/Capacity | O(1) | O(1) | Simple field access |
| Sort | O(n log n) | O(n²) | Iterative quicksort |
| Reset | O(1) | O(1) | Doesn't free memory |

### Space Complexity

- **Storage**: O(n) where n is capacity
- **Growth**: 2x when full (e.g., 4 → 8 → 16 → 32)
- **Shrink**: 0.5x when usage < 25% of capacity
- **Minimum**: Never shrinks below 4 elements

### Memory Overhead

- Per vector: ~32-40 bytes (struct overhead)
- Per element: Element size only (no per-element overhead)
- Unused capacity: Between 25% and 100% of used space

## Growth and Shrink Behavior

### Growth Strategy
```
Initial capacity: 4
Push when full: capacity *= 2
Example: 4 → 8 → 16 → 32 → 64 → 128
```

### Shrink Strategy
```
Shrink trigger: elem_count < capacity / 4
Shrink amount: capacity /= 2
Minimum: Never below 4
Example: 64 → 32 → 16 → 8 → 4 (stops)
```

### Example Scenario
```c
cvec_construct(v, int);
// capacity = 4, size = 0

cvec_push(v, 1); cvec_push(v, 2);
cvec_push(v, 3); cvec_push(v, 4);
// capacity = 4, size = 4

cvec_push(v, 5);
// capacity = 8, size = 5 (grew 2x)

// ... remove elements ...
// size = 1, capacity = 8

cvec_pop(v);
// size = 0, capacity = 4 (shrank 0.5x)
```

## Type Safety

The macro layer provides compile-time type checking:

```c
cvec_construct(numbers, int);
cvec_construct(names, char*);

cvec_push(numbers, 42);      // OK
cvec_push(numbers, "text");  // Compile error: type mismatch
cvec_push(names, "Alice");   // OK
cvec_push(names, 42);        // Compile error: type mismatch

int n = cvec_at(numbers, 0); // OK: int
char *s = cvec_at(names, 0); // OK: char*
```

## Sorting Integration

The vector integrates seamlessly with the csort library:

```c
// Integers - automatic comparator selection
cvec_construct(numbers, int);
cvec_push(numbers, 5);
cvec_push(numbers, 2);
cvec_push(numbers, 8);
cvec_sort(numbers);  // Uses csort_default_int_comparison_proc

// Strings - automatic string comparator
cvec_construct(names, char*);
cvec_push(names, "Charlie");
cvec_push(names, "Alice");
cvec_push(names, "Bob");
cvec_sort(names);  // Uses csort_default_string_comparison_proc

// Custom types - provide comparator
typedef struct { int id; char name[32]; } Person;
cvec_construct(people, Person);

int compare_by_id(const void *a, const void *b) {
    return ((Person*)a)->id - ((Person*)b)->id;
}
cvec_sort_with_comparator(people, compare_by_id);
```

## Custom Memory Management

Use custom allocators for specialized scenarios:

```c
ccol_memmgmt_procs_t arena = {
    .malloc = arena_malloc,
    .calloc = arena_calloc,
    .realloc = arena_realloc,
    .free = arena_free
};

cvec_construct(v, int);
cvec_init_with_mprocs(v, &arena);

// All allocations use arena allocator
cvec_push(v, 42);
```

## Common Patterns

### Building from Array

```c
int data[] = {1, 2, 3, 4, 5};
cvec_construct(v, int);

for (size_t i = 0; i < sizeof(data)/sizeof(data[0]); i++) {
    cvec_push(v, data[i]);
}
```

### Filter Elements

```c
cvec_construct(src, int);
cvec_construct(dst, int);

// Populate src...

for (size_t i = 0; i < cvec_size(src); i++) {
    int val = cvec_at(src, i);
    if (val % 2 == 0) {  // Keep even numbers
        cvec_push(dst, val);
    }
}

cvec_destroy(src);
cvec_destroy(dst);
```

### Transform Elements

```c
cvec_construct(numbers, int);
// Populate numbers...

for (size_t i = 0; i < cvec_size(numbers); i++) {
    int *ptr = cvec_at_ptr(numbers, i);
    *ptr *= 2;  // Double each element in-place
}
```

### Stack Operations

```c
cvec_construct(stack, int);

// Push
cvec_push(stack, 10);
cvec_push(stack, 20);
cvec_push(stack, 30);

// Pop
while (cvec_size(stack) > 0) {
    int val = cvec_pop(stack);
    printf("Popped: %d\n", val);
}
```

## Thread Safety

**Vectors are NOT thread-safe by default.** External synchronization is required for concurrent access:

```c
cvec_construct(shared_vec, int);
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Thread-safe push
pthread_mutex_lock(&lock);
cvec_push(shared_vec, value);
pthread_mutex_unlock(&lock);

// Thread-safe access
pthread_mutex_lock(&lock);
int val = cvec_at(shared_vec, index);
pthread_mutex_unlock(&lock);
```

## Best Practices

1. **Use type-safe macros** - Prefer `cvec_push()` over `cvector_push_back()`
2. **Reserve capacity if known** - Reduces reallocations (use cvector_ensure_capacity)
3. **Prefer cvec_construct** - Automatic cleanup with _ccol_destructor attribute
4. **Check size before access** - Avoid out-of-bounds indexing
5. **Use cvec_at_ptr for modifications** - More efficient than get-modify-set
6. **Don't hold pointers across push/pop** - Reallocation invalidates pointers

## Memory Efficiency Tips

### Pre-allocate for Known Sizes
```c
cvec_construct(v, int);
// If you know you'll need ~100 elements
cvector_ensure_capacity(v, 100);
// Now 100 pushes won't trigger reallocation
```

### Shrink After Bulk Removal
```c
// After removing many elements
if (cvec_size(v) < cvec_capacity(v) / 4) {
    // Force shrink by popping/pushing
    // (automatic shrink happens at 25% utilization)
}
```

### Use Reset to Keep Capacity
```c
// Clear elements but keep allocated memory
cvec_reset(v);
// Useful for reusing vector in loops
```

## Common Pitfalls

### ❌ Holding Pointers Across Modifications
```c
int *ptr = cvec_at_ptr(v, 0);
cvec_push(v, 42);  // May realloc!
*ptr = 99;         // DANGER: ptr may be invalid
```

### ✅ Correct Approach
```c
cvec_push(v, 42);
int *ptr = cvec_at_ptr(v, 0);
*ptr = 99;  // Safe: no modifications after getting pointer
```

### ❌ Out of Bounds Access
```c
cvec_construct(v, int);
int x = cvec_at(v, 0);  // DANGER: empty vector
```

### ✅ Correct Approach
```c
cvec_construct(v, int);
if (cvec_size(v) > 0) {
    int x = cvec_at(v, 0);
}
```

## Integration

```c
// Include in your project
#include "cvector.h"

// Requires csort.h for sorting functionality
#include "csort.h"

// Compile
// gcc myprogram.c cvector.c csort.c -o myprogram
```

## Comparison with Other Data Structures

| Feature | cvector | Static Array | Linked List |
|---------|---------|--------------|-------------|
| Random Access | O(1) | O(1) | O(n) |
| Insert at End | O(1)* | N/A | O(1) |
| Insert at Middle | O(n) | N/A | O(1) |
| Memory Overhead | Low | None | High |
| Cache Friendly | Yes | Yes | No |
| Dynamic Size | Yes | No | Yes |

*Amortized O(1) for push

## License

MIT License - See source file header for full license text.

## Related Components

- `csort.h` - Sorting library (used for cvec_sort)
- `common.h` - Common types and utilities
- `cmempool.h` - Memory pool (alternative to dynamic allocation)
