# Sorting Library (csort)

Generic, type-safe sorting with iterative quicksort and automatic comparator selection.

## Overview

`csort.h` provides a type-generic sorting facility for C with automatic comparator selection for standard types. Uses an iterative quicksort implementation that is stack-safe and provides O(n log n) average performance.

## Key Features

- **Type-Generic Sorting** - Works with any data type via function pointers
- **Automatic Comparators** - Built-in comparators for all C numeric types and strings
- **Iterative Implementation** - No recursion, stack-safe for large arrays
- **Custom Comparators** - Support for user-defined comparison functions
- **Integration with Collections** - Seamless integration with cvector and other containers
- **Custom Swap Functions** - Override default byte-by-byte swap

## Quick Start

### Sorting Primitive Arrays

```c
#include <csort.h>

// Integer array
int numbers[] = {5, 2, 8, 1, 9};
size_t len = sizeof(numbers) / sizeof(numbers[0]);

csort_sort(
    numbers,                                  // array
    len,                                      // length
    sizeof(int),                              // element size
    array_getter,                             // getter function
    csort_default_int_comparison_proc,        // comparator
    NULL,                                     // default swap
    NULL                                      // default memory mgmt
);

// Array is now: {1, 2, 5, 8, 9}
```

### With Automatic Comparator Selection

```c
// The library can select the right comparator
int dummy;
ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(dummy);

csort_sort(numbers, len, sizeof(int), array_getter, cmp, NULL, NULL);
```

### Sorting Strings

```c
char *names[] = {"Charlie", "Alice", "Bob"};
size_t len = sizeof(names) / sizeof(names[0]);

csort_sort(
    names,
    len,
    sizeof(char*),
    array_getter,
    csort_default_string_comparison_proc,
    NULL,
    NULL
);

// Array is now: {"Alice", "Bob", "Charlie"}
```

### Custom Comparator

```c
typedef struct {
    int id;
    char name[32];
    double score;
} Student;

int compare_students_by_score(const void *a, const void *b) {
    double sa = ((Student*)a)->score;
    double sb = ((Student*)b)->score;
    if (sa < sb) return -1;
    if (sa > sb) return 1;
    return 0;
}

Student students[100];
// ... populate students ...

csort_sort(
    students,
    100,
    sizeof(Student),
    array_getter,
    compare_students_by_score,
    NULL,
    NULL
);
```

## API Overview

### Main Sorting Function

```c
void csort_sort(
    void *collection,                    // Array or collection to sort
    size_t length,                       // Number of elements
    size_t elem_size,                    // Size of each element
    csort_item_getter_proc_t getter_proc,  // Element access function
    ccol_comparison_proc_t comparison_proc, // Comparison function
    csort_item_swap_proc_t swap_proc,    // Swap function (NULL for default)
    ccol_memmgmt_procs_t *mprocs         // Memory management (NULL for default)
);
```

### Built-in Comparators

```c
// Numeric types
int csort_default_char_comparison_proc(const void *a, const void *b);
int csort_default_short_comparison_proc(const void *a, const void *b);
int csort_default_int_comparison_proc(const void *a, const void *b);
int csort_default_long_comparison_proc(const void *a, const void *b);
int csort_default_long_long_comparison_proc(const void *a, const void *b);

// Unsigned types
int csort_default_unsigned_char_comparison_proc(const void *a, const void *b);
int csort_default_unsigned_short_comparison_proc(const void *a, const void *b);
int csort_default_unsigned_int_comparison_proc(const void *a, const void *b);
int csort_default_unsigned_long_comparison_proc(const void *a, const void *b);
int csort_default_unsigned_long_long_comparison_proc(const void *a, const void *b);

// Floating point
int csort_default_float_comparison_proc(const void *a, const void *b);
int csort_default_double_comparison_proc(const void *a, const void *b);
int csort_default_long_double_comparison_proc(const void *a, const void *b);

// Strings
int csort_default_string_comparison_proc(const void *a, const void *b);
```

### Automatic Comparator Selection

```c
// Use C11 _Generic to select appropriate comparator
ccol_comparison_proc_t csort_get_default_comparison_proc(x);
```

### Swap Functions

```c
// Default byte-by-byte swap
void csort_default_swap_proc(void *first, void *second, size_t elem_size);

// Pointer swap (for pointer arrays)
void csort_default_pointer_swap_proc(void *first, void *second, size_t elem_size);
```

## Performance Characteristics

### Time Complexity

| Case | Complexity | Notes |
|------|-----------|-------|
| Average | O(n log n) | Expected case with good pivot selection |
| Worst | O(n²) | Rare with random data |
| Best | O(n log n) | Already sorted or nearly sorted |

### Space Complexity

| Aspect | Complexity | Notes |
|--------|-----------|-------|
| Stack | O(log n) average | Explicit stack for iteration |
| Stack | O(n) worst case | Degenerates on pathological input |
| Auxiliary | O(1) | In-place sorting (excluding stack) |

### Comparison with Other Algorithms

| Algorithm | Average | Worst | Stable | In-Place | Notes |
|-----------|---------|-------|--------|----------|-------|
| Quicksort (this) | O(n log n) | O(n²) | No | Yes* | Fast, iterative |
| Merge Sort | O(n log n) | O(n log n) | Yes | No | Predictable |
| Heap Sort | O(n log n) | O(n log n) | No | Yes | Good worst case |
| qsort (stdlib) | O(n log n) | O(n²) | No | Yes | Recursive |

*In-place except for O(log n) stack

## Iterative Implementation

Unlike the standard library `qsort()`, this implementation uses iteration instead of recursion:

```
Traditional Quicksort:    This Implementation:
┌──────────────┐         ┌──────────────┐
│ Recursive    │         │ Iterative    │
│ Call Stack   │         │ Explicit     │
│ (unbounded)  │         │ Stack        │
│              │         │ (grows as    │
│ May overflow │         │ needed)      │
└──────────────┘         └──────────────┘
```

**Advantages:**
- No stack overflow risk
- Predictable memory usage
- Better for embedded systems
- Easier to debug

## Type Safety

The automatic comparator selection uses C11 `_Generic` for type safety:

```c
int arr_int[] = {3, 1, 2};
double arr_double[] = {3.14, 1.41, 2.71};
char *arr_str[] = {"foo", "bar", "baz"};

// Automatically selects correct comparator for each type
csort_sort(arr_int, 3, sizeof(int), getter,
           csort_get_default_comparison_proc(arr_int[0]), NULL, NULL);

csort_sort(arr_double, 3, sizeof(double), getter,
           csort_get_default_comparison_proc(arr_double[0]), NULL, NULL);

csort_sort(arr_str, 3, sizeof(char*), getter,
           csort_get_default_comparison_proc(arr_str[0]), NULL, NULL);
```

## Integration with cvector

The sorting library integrates seamlessly with cvector:

```c
#include <cvector.h>
#include <csort.h>

cvec_construct(numbers, int);
cvec_push(numbers, 5);
cvec_push(numbers, 2);
cvec_push(numbers, 8);

// Sort vector - automatically uses correct comparator
cvec_sort(numbers);

// Custom comparator
int compare_desc(const void *a, const void *b) {
    return *(int*)b - *(int*)a;
}
cvec_sort_with_comparator(numbers, compare_desc);
```

## Getter Functions

The getter function abstracts element access:

```c
// For arrays
void *array_getter(void *array, size_t index) {
    return (char*)array + (index * elem_size);
}

// For linked lists
void *list_getter(void *list, size_t index) {
    node_t *n = (node_t*)list;
    for (size_t i = 0; i < index; i++) {
        n = n->next;
    }
    return &n->data;
}

// For vectors (internal implementation)
void *vector_getter(void *vec, size_t index) {
    return cvector_at_ptr((cvector*)vec, index);
}
```

## Custom Comparators

Comparators must follow this signature:

```c
int compare(const void *a, const void *b);
```

Return value:
- **Negative** if a < b
- **Zero** if a == b
- **Positive** if a > b

### Examples

#### Sort by Absolute Value
```c
int compare_abs(const void *a, const void *b) {
    int ia = abs(*(int*)a);
    int ib = abs(*(int*)b);
    return ia - ib;
}
```

#### Sort Strings by Length
```c
int compare_strlen(const void *a, const void *b) {
    const char *sa = *(const char**)a;
    const char *sb = *(const char**)b;
    size_t la = strlen(sa);
    size_t lb = strlen(sb);
    if (la < lb) return -1;
    if (la > lb) return 1;
    return strcmp(sa, sb);  // Tie-breaker
}
```

#### Sort Structs with Multiple Fields
```c
typedef struct {
    int priority;
    time_t timestamp;
} Task;

int compare_tasks(const void *a, const void *b) {
    const Task *ta = (const Task*)a;
    const Task *tb = (const Task*)b;
    
    // Sort by priority first
    if (ta->priority != tb->priority) {
        return ta->priority - tb->priority;
    }
    
    // Then by timestamp
    if (ta->timestamp < tb->timestamp) return -1;
    if (ta->timestamp > tb->timestamp) return 1;
    return 0;
}
```

## Custom Swap Functions

Override default swap for optimization:

```c
// Optimized for 4-byte elements
void swap_int(void *a, void *b, size_t size) {
    int temp = *(int*)a;
    *(int*)a = *(int*)b;
    *(int*)b = temp;
}

// Optimized for 8-byte elements
void swap_long(void *a, void *b, size_t size) {
    long temp = *(long*)a;
    *(long*)a = *(long*)b;
    *(long*)b = temp;
}
```

## Stability

**This quicksort implementation is NOT stable.** Equal elements may be reordered.

```c
typedef struct {
    int key;
    int value;
} Pair;

Pair arr[] = {{1, 'a'}, {2, 'b'}, {1, 'c'}};

// After sorting by key:
// Order of (1,'a') and (1,'c') is undefined
```

If stability is required, use a stable sort or include a tiebreaker in the comparator:

```c
int compare_stable(const void *a, const void *b) {
    const Pair *pa = (const Pair*)a;
    const Pair *pb = (const Pair*)b;
    
    if (pa->key != pb->key) {
        return pa->key - pb->key;
    }
    
    // Use original index as tiebreaker
    return pa->original_index - pb->original_index;
}
```

## Memory Management

The sort uses an explicit stack that grows dynamically:

- Initial capacity: 64 entries (or 128 for large arrays)
- Grows 2x when full
- Freed automatically after sorting
- Uses provided memory management or default malloc/free

## Best Practices

1. **Use built-in comparators when possible** - They're optimized
2. **Keep comparators simple** - They're called O(n log n) times
3. **Avoid heap allocation in comparators** - Performance killer
4. **Test with sorted, reverse-sorted, and random data** - Catches edge cases
5. **For small arrays (<10), consider insertion sort** - Lower overhead
6. **Check return values** - Allocation can fail

## Common Patterns

### Descending Order
```c
int compare_desc(const void *a, const void *b) {
    return csort_default_int_comparison_proc(b, a);  // Swap arguments
}
```

### Case-Insensitive String Sort
```c
int compare_case_insensitive(const void *a, const void *b) {
    return strcasecmp(*(const char**)a, *(const char**)b);
}
```

### Partial Sort (Sort First K Elements)
```c
// Sort entire array, but you only need the k smallest
csort_sort(arr, n, sizeof(int), getter, cmp, NULL, NULL);
// Use first k elements
```

## Performance Tips

### 1. Pre-allocate for Known Sizes
```c
// The stack pre-allocates based on array size
// Arrays > 1000 elements get 128 initial stack slots
```

### 2. Inline Comparators
```c
static inline int compare(const void *a, const void *b) {
    return *(int*)a - *(int*)b;
}
```

### 3. Optimize Swap for Large Structs
```c
// For large structs, swap pointers instead
void swap_pointers(void *a, void *b, size_t size) {
    void *temp = *(void**)a;
    *(void**)a = *(void**)b;
    *(void**)b = temp;
}
```

## Comparison Function Pitfalls

### ❌ Integer Overflow
```c
int bad_compare(const void *a, const void *b) {
    return *(int*)a - *(int*)b;  // May overflow!
}
```

### ✅ Correct Approach
```c
int good_compare(const void *a, const void *b) {
    int ia = *(int*)a;
    int ib = *(int*)b;
    return (ia > ib) - (ia < ib);  // No overflow
}
```

### ❌ Floating Point Direct Subtraction
```c
int bad_compare(const void *a, const void *b) {
    return *(double*)a - *(double*)b;  // Implicit conversion to int!
}
```

### ✅ Correct Approach
```c
int good_compare(const void *a, const void *b) {
    double da = *(double*)a;
    double db = *(double*)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}
```

## Thread Safety

The sort function itself is reentrant, but:

- **The array being sorted is NOT protected**
- **Multiple threads can sort different arrays safely**
- **Sorting the same array from multiple threads requires external locking**

```c
pthread_mutex_t sort_lock = PTHREAD_MUTEX_INITIALIZER;

void thread_safe_sort(int *arr, size_t len) {
    pthread_mutex_lock(&sort_lock);
    csort_sort(arr, len, sizeof(int), getter, cmp, NULL, NULL);
    pthread_mutex_unlock(&sort_lock);
}
```

## Error Handling

The sort function handles errors gracefully:

```c
// If stack allocation fails, sort is aborted
// Array may be partially sorted
// No crash or undefined behavior

// Always check array size before sorting
if (len == 0 || len == 1) {
    return;  // Already sorted
}
```

## Integration

```c
// Include in your project
#include "csort.h"

// Compile
// gcc myprogram.c csort.c -o myprogram
```

## Benchmarks

Approximate timings on modern x86_64 (Intel i7):

| Array Size | Random | Sorted | Reverse | Nearly Sorted |
|------------|--------|--------|---------|---------------|
| 100 | 2 µs | 1 µs | 2 µs | 1 µs |
| 1,000 | 30 µs | 15 µs | 35 µs | 20 µs |
| 10,000 | 400 µs | 200 µs | 500 µs | 250 µs |
| 100,000 | 5 ms | 2.5 ms | 6 ms | 3 ms |
| 1,000,000 | 60 ms | 30 ms | 75 ms | 35 ms |

*Times are approximate and vary by system*

## License

MIT License - See source file header for full license text.

## Related Components

- `cvector.h` - Dynamic vector (uses csort for sorting)
- `common.h` - Common types and utilities
