# Memory Pool Library (cmempool)

Fast, deterministic memory allocation with fixed-size and ranged memory pools.

## Overview

`cmempool.h` provides two types of memory pool allocators designed for high-performance scenarios where dynamic allocation overhead is unacceptable:

- **Fixed-Size Pool (mempool)** - Pre-allocated pool of fixed-size buffers with O(1) alloc/free
- **Ranged Pool (r_mempool)** - Multiple pools covering power-of-2 size ranges

Both eliminate fragmentation, provide deterministic allocation times, and support preallocated buffers for embedded systems.

## Key Features

- **O(1) Allocation & Deallocation** - Constant-time operations via free lists
- **Zero Fragmentation** - Fixed-size allocations prevent memory fragmentation
- **Preallocated Buffers** - Support for static memory in embedded systems
- **Thread-Safe or Single-Threaded** - Configurable locking overhead
- **Dynamic Fallback** - Optional heap allocation when pool exhausted
- **Corruption Detection** - Magic values and assertions detect double-free and corruption

## Quick Start

### Fixed-Size Memory Pool

```c
#include <cmempool.h>

// Create pool: 100 buffers of 256 bytes each
char *err = NULL;
mempool *mp = mempool_create(
    100,           // element count
    256,           // element size
    false,         // no dynamic fallback
    false,         // thread-safe
    NULL,          // default malloc/free
    &err
);

// Allocate entry (like malloc)
void *buffer = mempool_alloc_entry(mp);

// Use buffer...
strcpy(buffer, "Hello from pool");

// Free entry (like free) - returns to pool
mempool_free_entry(buffer);

// Query statistics
printf("Total capacity: %zu\n", mempool_total_capacity(mp));
printf("Used: %zu\n", mempool_used_count(mp));
printf("Dynamic allocs: %zu\n", mempool_dynamic_allocs_count(mp));

// Cleanup
mempool_destroy(mp);
```

### Ranged Memory Pool

```c
// Create ranged pool: sizes from 2^4 (16) to 2^10 (1024) bytes
// Smallest size pool has 2^8 (256) elements
r_mempool *rmp = r_mempool_create(
    4,             // smallest size: 2^4 = 16 bytes
    10,            // largest size: 2^10 = 1024 bytes
    8,             // smallest pool elem count: 2^8 = 256
    fallback_disabled,
    false,         // thread-safe
    NULL,
    &err
);

// Allocate any size within range
void *buf1 = r_mempool_alloc_entry(rmp, 32);   // From 32-byte pool
void *buf2 = r_mempool_alloc_entry(rmp, 100);  // From 128-byte pool
void *buf3 = r_mempool_alloc_entry(rmp, 500);  // From 512-byte pool

// Use buffers...

// Free returns to appropriate pool
mempool_free_entry(buf1);
mempool_free_entry(buf2);
mempool_free_entry(buf3);

r_mempool_destroy(rmp);
```

### Preallocated Buffer (Embedded Systems)

```c
// Declare static buffer at compile time
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(my_buffer, 50, 128);

// Create pool from preallocated buffer
mempool *mp = mempool_create_from_preallocated_buffer(
    my_buffer,
    sizeof(my_buffer),
    128,           // element size
    false,         // no fallback (buffer is fixed)
    true,          // single-threaded (typical for embedded)
    NULL,
    &err
);

// Use normally
void *entry = mempool_alloc_entry(mp);
// ...
mempool_free_entry(entry);

// Destroy pool (buffer itself remains - it's static)
mempool_destroy(mp);
```

## Fixed-Size Pool API

### Creation & Destruction

```c
// Create pool
mempool *mempool_create(
    size_t elem_count,
    size_t elem_size,
    bool fallback_to_dynamic_memory,
    bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs,
    char **err
);

// Create from preallocated buffer
mempool *mempool_create_from_preallocated_buffer(
    void *buffer,
    size_t buf_size,
    size_t elem_size,
    bool fallback_to_dynamic_memory,
    bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs,
    char **err
);

// Destroy pool
void mempool_destroy(mempool *mp);
```

### Allocation & Deallocation

```c
// Allocate entry (uninitialized)
void *mempool_alloc_entry(mempool *mp);

// Allocate entry (zeroed)
void *mempool_calloc_entry(mempool *mp);

// Free entry
void mempool_free_entry(void *entry);
```

### Queries

```c
// Get total capacity
size_t mempool_total_capacity(mempool *mp);

// Get number used
size_t mempool_used_count(mempool *mp);

// Get dynamic allocation count (if fallback enabled)
size_t mempool_dynamic_allocs_count(mempool *mp);
```

## Ranged Pool API

### Creation & Destruction

```c
// Create ranged pool
r_mempool *r_mempool_create(
    uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two,
    uint8_t smallest_elem_count_power_of_two,
    r_memory_fallback_policy_t fb_policy,
    bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs,
    char **err
);

// Create from preallocated buffer
r_mempool *r_mempool_create_from_preallocated_buffer(
    void *buffer,
    size_t buf_size,
    uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two,
    uint8_t smallest_elem_count_power_of_two,
    r_memory_fallback_policy_t fb_policy,
    bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs,
    char **err
);

// Destroy ranged pool
void r_mempool_destroy(r_mempool *rmp);
```

### Allocation & Deallocation

```c
// Allocate size bytes (uninitialized)
void *r_mempool_alloc_entry(r_mempool *rmp, size_t size);

// Allocate size bytes (zeroed)
void *r_mempool_calloc_entry(r_mempool *rmp, size_t size);

// Reallocate to new size (may move data)
void *r_mempool_realloc_entry(r_mempool *rmp, void *addr, size_t size);

// Free entry
void r_mempool_free_entry(void *entry);
```

### Queries

```c
// Get capacity for specific size
size_t r_mempool_total_capacity(r_mempool *rmp, size_t size);

// Get used count for specific size
size_t r_mempool_used_count(r_mempool *rmp, size_t size);

// Get dynamic alloc count for specific size
size_t r_mempool_dynamic_allocs_count(r_mempool *rmp, size_t size);
```

## Fallback Policies

Memory pools support three fallback behaviors when exhausted:

### fallback_disabled
- Allocations fail when pool is full
- Returns NULL
- Most deterministic behavior

### fallback_at_first_exhaustion (Fixed-size only)
- Each allocation attempt falls back to heap if pool full
- Tracks dynamic allocations separately
- Mixed pool/heap usage

### fallback_at_first_exhaustion (Ranged pool)
- Each size pool has its own fallback
- Exhausted pool allocates from heap
- Other pools unaffected

### fallback_at_last_exhaustion (Ranged pool only)
- Only falls back after all pools exhausted
- Tries progressively larger pools first
- Minimizes heap allocation

## Performance Characteristics

### Fixed-Size Pool

| Operation | Time | Space | Notes |
|-----------|------|-------|-------|
| Allocate | O(1) | - | Pop from free list |
| Free | O(1) | - | Push to free list |
| Create | O(n) | O(n) | Initialize n elements |
| Destroy | O(1) | - | Single free call |

### Ranged Pool

| Operation | Time | Space | Notes |
|-----------|------|-------|-------|
| Allocate | O(1) | - | Maps size to pool |
| Free | O(1) | - | Uses embedded pool ptr |
| Realloc | O(1) or O(n) | - | O(1) if same pool, O(n) if copy needed |
| Create | O(n) | O(n) | Create all sub-pools |

### Memory Overhead

**Fixed-Size Pool:**
- Per pool: ~80 bytes
- Per element: ~24 bytes header (hidden from user)
- Total: pool_struct + n * (elem_size + 24)

**Ranged Pool:**
- Per pool: ~64 bytes + sub-pools
- Sub-pools: (largest - smallest + 1) fixed pools
- Example: 4→10 creates 7 pools (16, 32, 64, 128, 256, 512, 1024)

## Ranged Pool Example

```c
// Create ranged pool: 16 to 1024 bytes, 256 smallest elements
r_mempool *rmp = r_mempool_create(4, 10, 8, fallback_disabled, false, NULL, &err);

/*
 * This creates 7 internal pools:
 * Pool 0: 2^4  =   16 bytes, 2^8 = 256 elements
 * Pool 1: 2^5  =   32 bytes, 2^7 = 128 elements
 * Pool 2: 2^6  =   64 bytes, 2^6 =  64 elements
 * Pool 3: 2^7  =  128 bytes, 2^5 =  32 elements
 * Pool 4: 2^8  =  256 bytes, 2^4 =  16 elements
 * Pool 5: 2^9  =  512 bytes, 2^3 =   8 elements
 * Pool 6: 2^10 = 1024 bytes, 2^2 =   4 elements
 */

// Allocations are rounded up to next pool size
void *p1 = r_mempool_alloc_entry(rmp, 10);   // Gets 16-byte buffer (Pool 0)
void *p2 = r_mempool_alloc_entry(rmp, 50);   // Gets 64-byte buffer (Pool 2)
void *p3 = r_mempool_alloc_entry(rmp, 200);  // Gets 256-byte buffer (Pool 4)
void *p4 = r_mempool_alloc_entry(rmp, 1000); // Gets 1024-byte buffer (Pool 6)

// Query specific pool
printf("64-byte pool used: %zu/%zu\n",
       r_mempool_used_count(rmp, 64),
       r_mempool_total_capacity(rmp, 64));
```

## Thread Safety

### Thread-Safe Mode (single_threaded = false)
- All operations protected by read-write locks
- Multiple threads can safely allocate/free
- Read locks for queries, write locks for alloc/free
- Small performance overhead (~100-200 ns)

### Single-Threaded Mode (single_threaded = true)
- No locking overhead
- Must ensure external synchronization if shared
- Fastest performance
- Typical for embedded or single-threaded apps

## Corruption Detection

Pools include multiple safety mechanisms:

1. **Magic Values** - Detect use-after-free and corruption
2. **Double-Free Detection** - Asserts on duplicate free
3. **Bounds Checking** - Validates addresses are from pool
4. **Alignment Verification** - Ensures proper address alignment

All safety checks trigger `assert(false)` on violation.

## Common Use Cases

### Network Packet Buffers
```c
// Fixed packet size
mempool *packet_pool = mempool_create(1000, 1500, false, false, NULL, NULL);

void *packet = mempool_alloc_entry(packet_pool);
// Fill packet...
send_packet(packet);
// Receive thread frees
mempool_free_entry(packet);
```

### String Pool
```c
// Various string lengths
r_mempool *string_pool = r_mempool_create(
    4, 10, 8,  // 16 to 1024 bytes
    fallback_at_last_exhaustion,
    false, NULL, NULL
);

char *str = r_mempool_alloc_entry(string_pool, strlen(input) + 1);
strcpy(str, input);
// ...
mempool_free_entry(str);
```

### Object Pool
```c
typedef struct {
    int id;
    char name[64];
    double value;
} Object;

mempool *obj_pool = mempool_create(100, sizeof(Object), false, false, NULL, NULL);

Object *obj = mempool_calloc_entry(obj_pool);  // Zeroed
obj->id = generate_id();
strcpy(obj->name, "example");
// ...
mempool_free_entry(obj);
```

### Embedded System (No Heap)
```c
// Statically allocated pool
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(buffer, 200, 64);

mempool *mp = mempool_create_from_preallocated_buffer(
    buffer, sizeof(buffer), 64,
    false,  // No fallback - static only
    true,   // Single-threaded
    NULL, NULL
);

// All allocations from static buffer
void *entry = mempool_alloc_entry(mp);
```

## Best Practices

1. **Size pools appropriately** - Monitor usage with query functions
2. **Use calloc for security** - Clear sensitive data automatically
3. **Always free entries** - Memory leaks if you don't
4. **Match alloc/free calls** - One free per alloc
5. **Don't mix allocators** - Can't free pool entry with free()
6. **Consider fallback carefully** - Disabling provides strict bounds
7. **Use single-threaded when possible** - Avoid locking overhead

## Common Patterns

### RAII-Style Cleanup
```c
#define pool_entry_destructor(entry_ptr) \
    mempool_free_entry(*(entry_ptr))

void process() {
    void *entry _ccol_destructor(pool_entry_destructor) = 
        mempool_alloc_entry(pool);
    // Entry automatically freed on scope exit
}
```

### Pool Allocation Wrapper
```c
typedef struct {
    mempool *pool;
    void *buffer;
} PoolBuffer;

PoolBuffer *pool_buffer_create(mempool *pool) {
    PoolBuffer *pb = malloc(sizeof(PoolBuffer));
    pb->pool = pool;
    pb->buffer = mempool_alloc_entry(pool);
    return pb;
}

void pool_buffer_destroy(PoolBuffer *pb) {
    mempool_free_entry(pb->buffer);
    free(pb);
}
```

## Common Pitfalls

### ❌ Double Free
```c
void *entry = mempool_alloc_entry(mp);
mempool_free_entry(entry);
mempool_free_entry(entry);  // CRASH: double free detected
```

### ❌ Using After Free
```c
void *entry = mempool_alloc_entry(mp);
mempool_free_entry(entry);
strcpy(entry, "data");  // UNDEFINED: use after free
```

### ❌ Freeing with Wrong Deallocator
```c
void *entry = mempool_alloc_entry(mp);
free(entry);  // CRASH: wrong deallocator
```

### ✅ Correct Usage
```c
void *entry = mempool_alloc_entry(mp);
strcpy(entry, "data");
mempool_free_entry(entry);
entry = NULL;  // Good practice
```

## Integration

```c
// Include in your project
#include "cmempool.h"

// Link with pthread if using thread-safe mode
// gcc myprogram.c cmempool.c -lpthread -o myprogram
```

## Buffer Size Calculation

### Fixed-Size Pool
```c
// Calculate buffer size for preallocated pool
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(buf, 100, 256);
// Size: 100 * (256 + sizeof(__internal_entry_header))
```

### Ranged Pool
```c
// Macro calculates exact size needed
DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(buf, 4, 10, 8);
// Size: sum of all sub-pool sizes
```

## Performance Comparison

| Allocator | Alloc Time | Free Time | Fragmentation | Deterministic |
|-----------|------------|-----------|---------------|---------------|
| malloc/free | 100-1000ns | 100-1000ns | Yes | No |
| mempool | 10-50ns | 10-50ns | No | Yes |
| tcmalloc | 50-200ns | 50-200ns | Low | No |
| jemalloc | 50-200ns | 50-200ns | Low | No |

*Times approximate, depend on system and allocator*

## License

MIT License - See source file header for full license text.

## Related Components

- `common.h` - Common types and utilities
- `cthreadcomm.h` - Thread communication (uses mempools internally)
