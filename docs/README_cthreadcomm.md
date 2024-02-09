# Thread Communication Library (cthreadcomm)

High-performance, zero-copy thread communication primitives for C applications.

## Overview

`cthreadcomm.h` provides three thread-safe communication abstractions for inter-thread data exchange:

- **Circular Queue** - Fixed-size, bounded FIFO queue with zero-copy semantics
- **Dynamic Queue** - Unbounded queue that grows dynamically as needed
- **Channel** - Bidirectional communication with ownership tracking

All implementations use zero-copy message passing where the sender allocates memory and transfers ownership to the receiver.

## Key Features

- **Zero-Copy Design** - Messages are allocated once and ownership is transferred
- **Thread-Safe** - All operations are protected by mutexes and condition variables
- **Blocking & Timed Operations** - Support for both blocking waits and timeouts
- **Send/Receive Enable/Disable** - Fine-grained control over queue state
- **Custom Memory Management** - Optional custom allocators for embedded systems

## Quick Start

### Circular Queue Example

```c
#include <cthreadcomm.h>

// Create a circular queue for 100 messages of 256 bytes each
char *err = NULL;
circular_queue *cq = circq_create(100, 256, false, NULL, &err);

// Producer thread: zero-copy send
void *producer(void *arg) {
    while (running) {
        // Allocate buffer from queue's memory pool
        void *msg = circq_alloc_send_buf(cq);
        if (msg) {
            // Fill message
            sprintf(msg, "Message %d", counter++);
            // Transfer ownership to queue
            circq_send_zc(cq, msg);
        }
    }
    return NULL;
}

// Consumer thread: zero-copy receive
void *consumer(void *arg) {
    while (running) {
        void *msg;
        // Receive transfers ownership to consumer
        if (circq_recv(cq, &msg) == ccol_success) {
            printf("Received: %s\n", (char*)msg);
            // Consumer must free the message
            circq_free_recv_buf(cq, msg);
        }
    }
    return NULL;
}

// Cleanup
circq_destroy(cq);
```

### Dynamic Queue Example

```c
// Create dynamic queue (grows as needed)
dynamic_queue *dq = dynmq_create(256, 1000, false, NULL, &err);

// Send with automatic queue growth
void *msg = dynmq_alloc_send_buf(dq);
strcpy(msg, "Dynamic message");
dynmq_send_zc(dq, msg);

// Receive
void *recv_msg;
dynmq_recv(dq, &recv_msg);
printf("%s\n", (char*)recv_msg);
dynmq_free_recv_buf(dq, recv_msg);

dynmq_destroy(dq);
```

### Channel Example

```c
// Create bidirectional channel
channel *ch = chan_create(256, 100, 100, false, NULL, &err);

// Thread A sends to B
void *msg = chan_alloc_send_buf(ch);
strcpy(msg, "Hello from A");
chan_send_zc(ch, msg);

// Thread B receives from A
void *recv_msg;
chan_recv(ch, &recv_msg);
printf("B received: %s\n", (char*)recv_msg);
chan_free_recv_buf(ch, recv_msg);

// Thread B replies to A
void *reply = chan_alloc_reply_buf(ch);
strcpy(reply, "ACK from B");
chan_reply_zc(ch, reply);

// Thread A receives reply
void *reply_msg;
chan_recv_reply(ch, &reply_msg);
printf("A received: %s\n", (char*)reply_msg);
chan_free_reply_buf(ch, reply_msg);

chan_destroy(ch);
```

## API Overview

### Circular Queue Operations

| Operation | Description | Blocking |
|-----------|-------------|----------|
| `circq_create()` | Create fixed-size queue | - |
| `circq_alloc_send_buf()` | Allocate send buffer | No |
| `circq_send_zc()` | Send zero-copy (blocking) | Yes |
| `circq_timed_send_zc()` | Send with timeout | Timed |
| `circq_recv()` | Receive message (blocking) | Yes |
| `circq_timed_recv()` | Receive with timeout | Timed |
| `circq_free_recv_buf()` | Free received buffer | No |
| `circq_enable_sending()` | Enable queue for sends | No |
| `circq_disable_sending()` | Disable queue for sends | No |
| `circq_msg_count()` | Get current message count | No |
| `circq_destroy()` | Destroy queue | - |

### Dynamic Queue Operations

Similar to circular queue but with:
- `dynmq_*` prefix
- Queue grows automatically when full
- `dynmq_max_elem_count()` to check capacity limit

### Channel Operations

- `chan_create()` - Create bidirectional channel
- `chan_send_zc()` / `chan_recv()` - Primary direction
- `chan_reply_zc()` / `chan_recv_reply()` - Reply direction
- Separate buffer allocation for each direction

## Performance Characteristics

### Circular Queue
- **Send/Receive**: O(1) - constant time operations
- **Memory**: O(n) - fixed allocation at creation
- **Best For**: High-throughput, predictable workloads

### Dynamic Queue
- **Send/Receive**: O(1) amortized - may reallocate on growth
- **Memory**: O(n) - grows dynamically up to max_elem_count
- **Best For**: Variable workloads, unknown peak sizes

### Channel
- **Send/Receive**: O(1) - two independent circular queues
- **Memory**: O(n) - fixed allocation for both directions
- **Best For**: Request-response patterns, bidirectional communication

## Thread Safety

All operations are thread-safe with the following guarantees:

- **Multiple Producers**: Safe - internal locking prevents race conditions
- **Multiple Consumers**: Safe - each message delivered to exactly one consumer
- **Mixed Access**: Safe - can call any operations from any thread

**Note**: Message contents themselves are not protected after ownership transfer. The receiver must ensure exclusive access to message data.

## Ownership Semantics

The zero-copy design follows strict ownership rules:

1. **Sender allocates** via `*_alloc_send_buf()`
2. **Sender transfers** via `*_send_zc()` - sender loses ownership
3. **Receiver gains ownership** via `*_recv()` - must eventually free
4. **Receiver frees** via `*_free_recv_buf()` - returns to pool

**Critical**: Never free a buffer you don't own, and always free buffers you receive.

## Blocking Behavior

### Blocking Operations
- Block when queue is full (send) or empty (receive)
- Wake up when condition changes or queue is disabled

### Timed Operations
- Block up to specified timeout
- Return `ccol_timed_out` if timeout expires

### Non-Blocking Alternatives
- Check `*_msg_count()` before sending/receiving
- Use timed operations with zero timeout

## Enable/Disable Mechanism

Queues can be dynamically disabled to gracefully shut down:

```c
// Disable sending - blocks new sends, wakes blocked senders
circq_disable_sending(cq);

// Drain remaining messages
void *msg;
while (circq_recv(cq, &msg) == ccol_success) {
    process_message(msg);
    circq_free_recv_buf(cq, msg);
}

// Re-enable if needed
circq_enable_sending(cq);
```

## Error Handling

All functions return `ccol_retval_t`:

- `ccol_success` - Operation succeeded
- `ccol_not_permitted` - Queue disabled or invalid state
- `ccol_timed_out` - Timed operation expired
- `ccol_invalid_args` - Invalid parameters
- `ccol_not_enough_memory` - Allocation failed
- `ccol_container_full` - Max capacity reached (dynamic queue)

Always check return values for robust error handling.

## Custom Memory Management

All communication primitives support custom allocators:

```c
ccol_memmgmt_procs_t custom_mm = {
    .malloc = my_malloc,
    .calloc = my_calloc,
    .realloc = my_realloc,
    .free = my_free
};

circular_queue *cq = circq_create(100, 256, false, &custom_mm, &err);
```

Useful for:
- Memory pools in embedded systems
- Instrumentation and debugging
- Deterministic allocation behavior

## Best Practices

1. **Always free received buffers** - Memory leaks occur if you forget
2. **Check return values** - Handle errors gracefully
3. **Size queues appropriately** - Too small causes blocking, too large wastes memory
4. **Use timed operations for deadlock prevention** - Avoid infinite waits
5. **Disable before destroy** - Ensures clean shutdown of threads
6. **Match buffer sizes to typical messages** - Reduces wasted space

## Common Patterns

### Producer-Consumer
```c
circular_queue *cq = circq_create(100, 256, false, NULL, NULL);
// Multiple producers and consumers can safely share the queue
```

### Request-Response
```c
channel *ch = chan_create(256, 100, 100, false, NULL, NULL);
// Use send/recv for requests, reply/recv_reply for responses
```

### Work Queue with Timeout
```c
void *msg = circq_alloc_send_buf(queue);
// ... fill message ...
if (circq_timed_send_zc(queue, msg, &timeout) == ccol_timed_out) {
    // Handle timeout - must free msg ourselves
    circq_free_send_buf(queue, msg);
}
```

## Integration

```c
// Include in your project
#include "cthreadcomm.h"

// Link against pthread
// gcc myprogram.c cthreadcomm.c -lpthread -o myprogram
```

## Thread Communication Pattern Comparison

| Pattern | Abstraction | Best Use Case |
|---------|-------------|---------------|
| Circular Queue | Single direction, fixed size | High-throughput pipelines |
| Dynamic Queue | Single direction, grows | Variable-rate producers |
| Channel | Bidirectional, fixed size | RPC-style request-response |

## License

MIT License - See source file header for full license text.

## Related Components

- `cmempool.h` - Memory pool allocator (used internally)
- `common.h` - Common types and utilities
