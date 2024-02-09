/*
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
*/

#pragma once

#include <common.h>
#include <time.h>

/**
 * @file cthreadcomm.h
 * @brief Thread-safe message passing primitives for inter-thread communication
 *
 * Provides three thread-safe abstractions for message passing:
 * - circular_queue: Fixed-size bounded queue with blocking backpressure
 * - dynamic_queue: Unbounded queue (linked list) with no blocking on send
 * - channel: Bidirectional communication between owner and worker threads
 *
 * All operations use zero-copy semantics for efficient pointer ownership
 * transfer.
 */

/** @brief Opaque handle to a circular queue */
typedef struct circular_queue circular_queue;

/** @brief Opaque handle to a dynamic queue */
typedef struct dynamic_queue dynamic_queue;

/** @brief Opaque handle to a bidirectional channel */
typedef struct channel channel;

/**
 * @brief Message structure for zero-copy message passing
 *
 * Messages transfer ownership of the data pointer. After a successful send,
 * the sender's data pointer is set to NULL. The receiver gains ownership
 * and is responsible for freeing the memory.
 */
typedef struct c_message_t {
  void *data;  /**< Pointer to message data (ownership transfers on send) */
  size_t size; /**< Size of data in bytes */
} c_message_t;

/* ========================================================================== */
/*                          CIRCULAR QUEUE FUNCTIONS                          */
/* ========================================================================== */

/**
 * @brief Create a circular queue with custom memory management
 *
 * Creates a fixed-size bounded queue that uses a circular buffer internally.
 * Send operations block when the queue is full until space becomes available.
 *
 * @param max_size Maximum number of messages the queue can hold (1 to
 * max_elem_count)
 * @param mmgmt_procs Custom memory management procedures, or NULL to use
 * default malloc/free
 * @param err_str Optional pointer to receive error string on failure (pass NULL
 * to ignore)
 *
 * @return Pointer to newly created circular queue, or NULL on failure
 *
 * @note All operations on this queue are thread-safe
 * @note The queue must be destroyed with circular_queue_destroy() when done
 *
 * @see circular_queue_create
 * @see circular_queue_destroy
 */
circular_queue *circular_queue_create_with_mprocs(
    size_t max_size, ccol_memmgmt_procs_t *mmgmt_procs, char **err_str);

/**
 * @brief Create a circular queue with default memory management
 *
 * Convenience macro that creates a circular queue using standard malloc/free.
 *
 * @param max_size Maximum number of messages the queue can hold
 * @param err_str Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created circular queue, or NULL on failure
 */
#define circular_queue_create(max_size, err_str) \
  circular_queue_create_with_mprocs(max_size, NULL, err_str)

/**
 * @brief Destroy a circular queue (internal function)
 *
 * @param cq Circular queue to destroy
 *
 * @warning Do not call directly - use circular_queue_destroy() macro instead
 */
void __circular_queue_destroy(circular_queue *cq);

/**
 * @brief Destroy a circular queue and set pointer to NULL
 *
 * Frees all resources associated with the queue. Any messages still in the
 * queue will have their data pointers leaked - drain the queue before
 * destroying.
 *
 * @param cq Circular queue to destroy (will be set to NULL after destruction)
 *
 * @warning Messages remaining in the queue are not automatically freed
 * @note Safe to call with NULL pointer
 */
#define circular_queue_destroy(cq) \
  do {                             \
    __circular_queue_destroy(cq);  \
    cq = NULL;                     \
  } while (0)

/**
 * @brief Send a message to the queue (blocking, zero-copy)
 *
 * Blocks until space is available in the queue, then transfers ownership of
 * the message data to the queue. The msg->data pointer is set to NULL on
 * success.
 *
 * @param cq Circular queue to send to
 * @param msg Message to send (data ownership transfers on success)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or msg is NULL, or if msg validation fails
 * @return ccol_not_permitted if sending has been disabled
 *
 * @note Blocks indefinitely until space is available or sending is disabled
 * @note After waking from blocking, rechecks if sending is still enabled
 * @note On failure, caller retains ownership of msg->data
 * @note msg->data == NULL with msg->size == 0 is valid (sentinel message)
 * @note msg->data == NULL with msg->size > 0 is invalid
 *
 * @see circq_try_send_zc
 * @see circq_timed_send_zc
 */
ccol_retval_t circq_send_zc(circular_queue *cq, c_message_t *msg);

/**
 * @brief Try to send a message without blocking (zero-copy)
 *
 * Attempts to send immediately. If the queue is full, returns immediately
 * with ccol_container_full. On success, transfers ownership of msg->data.
 *
 * @param cq Circular queue to send to
 * @param msg Message to send (data ownership transfers on success)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or msg is NULL, or if msg validation fails
 * @return ccol_not_permitted if sending has been disabled
 * @return ccol_container_full if queue is full
 *
 * @note Never blocks
 * @note On failure, caller retains ownership of msg->data
 *
 * @see circq_send_zc
 * @see circq_timed_send_zc
 */
ccol_retval_t circq_try_send_zc(circular_queue *cq, c_message_t *msg);

/**
 * @brief Send a message with timeout (blocking, zero-copy)
 *
 * Blocks up to the specified timeout waiting for space. If space becomes
 * available, sends the message and transfers ownership. If timeout expires
 * or sending is disabled, returns with appropriate error code.
 *
 * @param cq Circular queue to send to
 * @param msg Message to send (data ownership transfers on success)
 * @param timeout Relative timeout duration (converted to absolute time
 * internally)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or msg is NULL, or if msg validation fails
 * @return ccol_not_permitted if sending has been disabled
 * @return ccol_timed_out if timeout expires before space becomes available
 * @return ccol_unexpected_failure on system error (check errno)
 *
 * @note Blocks up to timeout duration
 * @note After waking, rechecks if sending is still enabled
 * @note On failure, caller retains ownership of msg->data
 * @note Uses CLOCK_REALTIME for timeout calculations
 *
 * @see circq_send_zc
 * @see circq_try_send_zc
 */
ccol_retval_t circq_timed_send_zc(circular_queue *cq, c_message_t *msg,
                                  struct timespec *timeout);

/**
 * @brief Receive a message from the queue (blocking, zero-copy)
 *
 * Blocks until a message is available, then transfers ownership of the
 * message data to the caller via target_buf.
 *
 * @param cq Circular queue to receive from
 * @param target_buf Buffer to receive the message (receives ownership of data)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or target_buf is NULL
 *
 * @note Blocks indefinitely until a message arrives
 * @note Caller receives ownership of target_buf->data and must free it
 * @note No disable mechanism for receiving (only sending can be disabled)
 *
 * @see circq_try_recv_zc
 * @see circq_timed_recv_zc
 */
ccol_retval_t circq_recv_zc(circular_queue *cq, c_message_t *target_buf);

/**
 * @brief Try to receive a message without blocking (zero-copy)
 *
 * Attempts to receive immediately. If the queue is empty, returns immediately
 * with ccol_container_empty. On success, transfers ownership of message data.
 *
 * @param cq Circular queue to receive from
 * @param target_buf Buffer to receive the message (receives ownership of data)
 *
 * @return ccol_success on success (caller must free target_buf->data)
 * @return ccol_invalid_args if cq or target_buf is NULL
 * @return ccol_container_empty if queue is empty
 *
 * @note Never blocks
 * @note On success, caller receives ownership of target_buf->data
 *
 * @see circq_recv_zc
 * @see circq_timed_recv_zc
 */
ccol_retval_t circq_try_recv_zc(circular_queue *cq, c_message_t *target_buf);

/**
 * @brief Receive a message with timeout (blocking, zero-copy)
 *
 * Blocks up to the specified timeout waiting for a message. If a message
 * arrives, receives it and transfers ownership. If timeout expires, returns
 * with ccol_timed_out.
 *
 * @param cq Circular queue to receive from
 * @param target_buf Buffer to receive the message (receives ownership of data)
 * @param timeout Relative timeout duration (converted to absolute time
 * internally)
 *
 * @return ccol_success on success (caller must free target_buf->data)
 * @return ccol_invalid_args if cq or target_buf is NULL
 * @return ccol_timed_out if timeout expires before message arrives
 * @return ccol_unexpected_failure on system error (check errno)
 *
 * @note Blocks up to timeout duration
 * @note On success, caller receives ownership of target_buf->data
 * @note Uses CLOCK_REALTIME for timeout calculations
 *
 * @see circq_recv_zc
 * @see circq_try_recv_zc
 */
ccol_retval_t circq_timed_recv_zc(circular_queue *cq, c_message_t *target_buf,
                                  struct timespec *timeout);

/**
 * @brief Disable sending on the queue
 *
 * Prevents new send operations and wakes all threads currently blocked in
 * send operations. Blocked senders will return with ccol_not_permitted.
 * Receiving is not affected.
 *
 * @param cq Circular queue to disable sending on
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq is NULL
 *
 * @note Wakes all blocked senders immediately via broadcast
 * @note Can be re-enabled with circq_enable_sending()
 * @note Does not affect receive operations
 *
 * @see circq_enable_sending
 */
ccol_retval_t circq_disable_sending(circular_queue *cq);

/**
 * @brief Enable sending on the queue
 *
 * Re-enables send operations that were previously disabled and wakes all
 * threads currently blocked waiting to send. Blocked senders can now proceed.
 *
 * @param cq Circular queue to enable sending on
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq is NULL
 *
 * @note Wakes all blocked senders via broadcast
 * @note Threads can immediately attempt to send if space is available
 *
 * @see circq_disable_sending
 */
ccol_retval_t circq_enable_sending(circular_queue *cq);

/**
 * @brief Get current message count in the queue
 *
 * Returns the number of messages currently stored in the queue.
 *
 * @param cq Circular queue to query
 *
 * @return Number of messages in queue, or (size_t)-1 on error
 *
 * @note Thread-safe snapshot of message count
 * @note Return value of (size_t)-1 indicates error (NULL queue)
 */
size_t circq_msg_count(circular_queue *cq);

/* ========================================================================== */
/*                          DYNAMIC QUEUE FUNCTIONS                           */
/* ========================================================================== */

/**
 * @brief Create a dynamic queue with custom memory management
 *
 * Creates an unbounded queue using a doubly-linked list. Send operations
 * never block (except on memory allocation failure). The queue can grow
 * up to max_elem_count messages.
 *
 * @param mmgmt_procs Custom memory management procedures, or NULL to use
 * default malloc/free
 * @param err_str Optional pointer to receive error string on failure (pass NULL
 * to ignore)
 *
 * @return Pointer to newly created dynamic queue, or NULL on failure
 *
 * @note Send operations never block (limited only by available memory)
 * @note Maximum size is max_elem_count (defined in common.h)
 * @note The queue must be destroyed with dynamic_queue_destroy() when done
 *
 * @see dynamic_queue_create
 * @see dynamic_queue_destroy
 */
dynamic_queue *dynamic_queue_create_with_mprocs(
    ccol_memmgmt_procs_t *mmgmt_procs, char **err_str);

/**
 * @brief Create a dynamic queue with default memory management
 *
 * Convenience macro that creates a dynamic queue using standard malloc/free.
 *
 * @param err_str Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created dynamic queue, or NULL on failure
 */
#define dynamic_queue_create(err_str) \
  dynamic_queue_create_with_mprocs(NULL, err_str)

/**
 * @brief Destroy a dynamic queue (internal function)
 *
 * @param dq Dynamic queue to destroy
 *
 * @warning Do not call directly - use dynamic_queue_destroy() macro instead
 */
void __dynamic_queue_destroy(dynamic_queue *dq);

/**
 * @brief Destroy a dynamic queue and set pointer to NULL
 *
 * Frees all resources associated with the queue including all linked list
 * nodes. Any messages still in the queue will have their data pointers leaked -
 * drain the queue before destroying.
 *
 * @param dq Dynamic queue to destroy (will be set to NULL after destruction)
 *
 * @warning Messages remaining in the queue are not automatically freed
 * @note Safe to call with NULL pointer
 */
#define dynamic_queue_destroy(dq) \
  do {                            \
    __dynamic_queue_destroy(dq);  \
    dq = NULL;                    \
  } while (0)

/**
 * @brief Send a message to the dynamic queue (non-blocking, zero-copy)
 *
 * Sends a message without blocking. Allocates a new list node and appends
 * the message to the tail of the queue. Transfers ownership of msg->data.
 *
 * @param dq Dynamic queue to send to
 * @param msg Message to send (data ownership transfers on success)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if dq or msg is NULL, or if msg validation fails
 * @return ccol_not_permitted if sending has been disabled
 * @return ccol_container_full if queue has reached max_elem_count
 * @return ccol_not_enough_memory if node allocation fails
 *
 * @note Never blocks (returns immediately with success or error)
 * @note On failure, caller retains ownership of msg->data
 * @note No blocking/timed send variants (queue is unbounded by design)
 * @note Limited only by max_elem_count and available memory
 *
 * @see dynmq_recv_zc
 */
ccol_retval_t dynmq_send_zc(dynamic_queue *dq, c_message_t *msg);

/**
 * @brief Receive a message from the dynamic queue (blocking, zero-copy)
 *
 * Blocks until a message is available, then removes it from the head of the
 * queue and transfers ownership to the caller.
 *
 * @param dq Dynamic queue to receive from
 * @param target_buf Buffer to receive the message (receives ownership of data)
 *
 * @return ccol_success on success (caller must free target_buf->data)
 * @return ccol_invalid_args if dq or target_buf is NULL
 *
 * @note Blocks indefinitely until a message arrives
 * @note Caller receives ownership of target_buf->data and must free it
 *
 * @see dynmq_try_recv_zc
 * @see dynmq_timed_recv_zc
 */
ccol_retval_t dynmq_recv_zc(dynamic_queue *dq, c_message_t *target_buf);

/**
 * @brief Try to receive a message without blocking (zero-copy)
 *
 * Attempts to receive immediately. If the queue is empty, returns immediately
 * with ccol_container_empty.
 *
 * @param dq Dynamic queue to receive from
 * @param target_buf Buffer to receive the message (receives ownership of data)
 *
 * @return ccol_success on success (caller must free target_buf->data)
 * @return ccol_invalid_args if dq or target_buf is NULL
 * @return ccol_container_empty if queue is empty
 *
 * @note Never blocks
 * @note On success, caller receives ownership of target_buf->data
 *
 * @see dynmq_recv_zc
 * @see dynmq_timed_recv_zc
 */
ccol_retval_t dynmq_try_recv_zc(dynamic_queue *dq, c_message_t *target_buf);

/**
 * @brief Receive a message with timeout (blocking, zero-copy)
 *
 * Blocks up to the specified timeout waiting for a message. If a message
 * arrives, receives it and transfers ownership.
 *
 * @param dq Dynamic queue to receive from
 * @param target_buf Buffer to receive the message (receives ownership of data)
 * @param timeout Relative timeout duration (converted to absolute time
 * internally)
 *
 * @return ccol_success on success (caller must free target_buf->data)
 * @return ccol_invalid_args if dq or target_buf is NULL
 * @return ccol_timed_out if timeout expires before message arrives
 * @return ccol_unexpected_failure on system error (check errno)
 *
 * @note Blocks up to timeout duration
 * @note On success, caller receives ownership of target_buf->data
 * @note Uses CLOCK_REALTIME for timeout calculations
 *
 * @see dynmq_recv_zc
 * @see dynmq_try_recv_zc
 */
ccol_retval_t dynmq_timed_recv_zc(dynamic_queue *dq, c_message_t *target_buf,
                                  struct timespec *timeout);

/**
 * @brief Disable sending on the dynamic queue
 *
 * Prevents new send operations. Unlike circular queues, dynamic queues have
 * no blocked senders (sends never block), so this only affects future send
 * attempts.
 *
 * @param dq Dynamic queue to disable sending on
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if dq is NULL
 *
 * @note Can be re-enabled with dynmq_enable_sending()
 * @note Does not affect receive operations
 *
 * @see dynmq_enable_sending
 */
ccol_retval_t dynmq_disable_sending(dynamic_queue *dq);

/**
 * @brief Enable sending on the dynamic queue
 *
 * Re-enables send operations that were previously disabled.
 *
 * @param dq Dynamic queue to enable sending on
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if dq is NULL
 *
 * @see dynmq_disable_sending
 */
ccol_retval_t dynmq_enable_sending(dynamic_queue *dq);

/**
 * @brief Get current message count in the dynamic queue
 *
 * Returns the number of messages currently stored in the queue.
 *
 * @param dq Dynamic queue to query
 *
 * @return Number of messages in queue, or (size_t)-1 on error
 *
 * @note Thread-safe snapshot of message count
 * @note Return value of (size_t)-1 indicates error (NULL queue)
 */
size_t dynmq_msg_count(dynamic_queue *dq);

/* ========================================================================== */
/*                            CHANNEL FUNCTIONS                               */
/* ========================================================================== */

/**
 * @brief Create a bidirectional channel with custom memory management
 *
 * Creates a channel with two internal circular queues for bidirectional
 * communication between an owner thread and worker threads. The creating
 * thread becomes the owner. Direction is automatically selected based on
 * the calling thread's ID.
 *
 * @param max_size Maximum number of messages each direction can hold
 * @param mmgmt_procs Custom memory management procedures, or NULL to use
 * default malloc/free
 * @param err_str Optional pointer to receive error string on failure (pass NULL
 * to ignore)
 *
 * @return Pointer to newly created channel, or NULL on failure
 *
 * @note Contains two circular queues: owner_to_workers and workers_to_owner
 * @note Creating thread is designated as owner; all others are workers
 * @note Direction is automatically selected based on thread ID
 * @note The channel must be destroyed with channel_destroy() when done
 *
 * @see channel_create
 * @see channel_destroy
 */
channel *channel_create_with_mprocs(size_t max_size,
                                    ccol_memmgmt_procs_t *mmgmt_procs,
                                    char **err_str);

/**
 * @brief Create a bidirectional channel with default memory management
 *
 * Convenience macro that creates a channel using standard malloc/free.
 *
 * @param max_size Maximum number of messages each direction can hold
 * @param err_str Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created channel, or NULL on failure
 */
#define channel_create(max_size, err_str) \
  channel_create_with_mprocs(max_size, NULL, err_str)

/**
 * @brief Destroy a channel (internal function)
 *
 * @param ch Channel to destroy
 *
 * @warning Do not call directly - use channel_destroy() macro instead
 */
void __channel_destroy(channel *ch);

/**
 * @brief Destroy a channel and set pointer to NULL
 *
 * Frees all resources associated with the channel including both internal
 * circular queues. Any messages still in either queue will have their data
 * pointers leaked - drain both directions before destroying.
 *
 * @param ch Channel to destroy (will be set to NULL after destruction)
 *
 * @warning Messages remaining in either direction are not automatically freed
 * @note Safe to call with NULL pointer
 */
#define channel_destroy(ch) \
  do {                      \
    __channel_destroy(ch);  \
    ch = NULL;              \
  } while (0)

/**
 * @brief Send a message through the channel (blocking, zero-copy)
 *
 * Automatically selects the appropriate queue based on calling thread:
 * - Owner thread sends to owner_to_workers queue
 * - Worker threads send to workers_to_owner queue
 *
 * @param ch Channel to send through
 * @param msg Message to send (data ownership transfers on success)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch or msg is NULL, or if msg validation fails
 * @return ccol_not_permitted if sending has been disabled on the selected
 * direction
 *
 * @note Blocks until space is available in the selected queue
 * @note Direction is automatically determined by thread ID
 * @note On failure, caller retains ownership of msg->data
 *
 * @see chan_try_send_zc
 * @see chan_timed_send_zc
 * @see chan_recv_zc
 */
ccol_retval_t chan_send_zc(channel *ch, c_message_t *msg);

/**
 * @brief Try to send a message without blocking (zero-copy)
 *
 * Attempts to send immediately through the automatically-selected queue.
 * Returns immediately if the queue is full.
 *
 * @param ch Channel to send through
 * @param msg Message to send (data ownership transfers on success)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch or msg is NULL, or if msg validation fails
 * @return ccol_not_permitted if sending has been disabled on the selected
 * direction
 * @return ccol_container_full if the selected queue is full
 *
 * @note Never blocks
 * @note Direction is automatically determined by thread ID
 *
 * @see chan_send_zc
 * @see chan_timed_send_zc
 */
ccol_retval_t chan_try_send_zc(channel *ch, c_message_t *msg);

/**
 * @brief Send a message with timeout (blocking, zero-copy)
 *
 * Blocks up to the specified timeout waiting for space in the automatically-
 * selected queue.
 *
 * @param ch Channel to send through
 * @param msg Message to send (data ownership transfers on success)
 * @param timeout Relative timeout duration (converted to absolute time
 * internally)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch or msg is NULL, or if msg validation fails
 * @return ccol_not_permitted if sending has been disabled on the selected
 * direction
 * @return ccol_timed_out if timeout expires before space becomes available
 * @return ccol_unexpected_failure on system error (check errno)
 *
 * @note Blocks up to timeout duration
 * @note Direction is automatically determined by thread ID
 * @note Uses CLOCK_REALTIME for timeout calculations
 *
 * @see chan_send_zc
 * @see chan_try_send_zc
 */
ccol_retval_t chan_timed_send_zc(channel *ch, c_message_t *msg,
                                 struct timespec *timeout);

/**
 * @brief Receive a message from the channel (blocking, zero-copy)
 *
 * Automatically selects the appropriate queue based on calling thread:
 * - Owner thread receives from workers_to_owner queue
 * - Worker threads receive from owner_to_workers queue
 *
 * @param ch Channel to receive from
 * @param target_buf Buffer to receive the message (receives ownership of data)
 *
 * @return ccol_success on success (caller must free target_buf->data)
 * @return ccol_invalid_args if ch or target_buf is NULL
 *
 * @note Blocks until a message arrives in the selected queue
 * @note Direction is automatically determined by thread ID
 * @note Caller receives ownership of target_buf->data and must free it
 *
 * @see chan_try_recv_zc
 * @see chan_timed_recv_zc
 * @see chan_send_zc
 */
ccol_retval_t chan_recv_zc(channel *ch, c_message_t *target_buf);

/**
 * @brief Try to receive a message without blocking (zero-copy)
 *
 * Attempts to receive immediately from the automatically-selected queue.
 * Returns immediately if the queue is empty.
 *
 * @param ch Channel to receive from
 * @param target_buf Buffer to receive the message (receives ownership of data)
 *
 * @return ccol_success on success (caller must free target_buf->data)
 * @return ccol_invalid_args if ch or target_buf is NULL
 * @return ccol_container_empty if the selected queue is empty
 *
 * @note Never blocks
 * @note Direction is automatically determined by thread ID
 *
 * @see chan_recv_zc
 * @see chan_timed_recv_zc
 */
ccol_retval_t chan_try_recv_zc(channel *ch, c_message_t *target_buf);

/**
 * @brief Receive a message with timeout (blocking, zero-copy)
 *
 * Blocks up to the specified timeout waiting for a message in the
 * automatically-selected queue.
 *
 * @param ch Channel to receive from
 * @param target_buf Buffer to receive the message (receives ownership of data)
 * @param timeout Relative timeout duration (converted to absolute time
 * internally)
 *
 * @return ccol_success on success (caller must free target_buf->data)
 * @return ccol_invalid_args if ch or target_buf is NULL
 * @return ccol_timed_out if timeout expires before message arrives
 * @return ccol_unexpected_failure on system error (check errno)
 *
 * @note Blocks up to timeout duration
 * @note Direction is automatically determined by thread ID
 * @note Uses CLOCK_REALTIME for timeout calculations
 *
 * @see chan_recv_zc
 * @see chan_try_recv_zc
 */
ccol_retval_t chan_timed_recv_zc(channel *ch, c_message_t *target_buf,
                                 struct timespec *timeout);

/**
 * @brief Direction specifier for channel control operations
 */
typedef enum channel_direction {
  owner_to_workers = 0, /**< Messages from owner to worker threads */
  workers_to_owner      /**< Messages from worker threads to owner */
} channel_direction;

/**
 * @brief Disable sending on a specific channel direction
 *
 * Prevents new send operations on the specified direction and wakes all
 * threads currently blocked in send operations on that direction.
 *
 * @param ch Channel to modify
 * @param d Direction to disable (owner_to_workers or workers_to_owner)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch is NULL or d is invalid
 *
 * @note Wakes all blocked senders on the specified direction
 * @note Can be re-enabled with chan_enable_sending()
 * @note Does not affect the opposite direction
 *
 * @see chan_enable_sending
 */
ccol_retval_t chan_disable_sending(channel *ch, channel_direction d);

/**
 * @brief Enable sending on a specific channel direction
 *
 * Re-enables send operations on the specified direction that were previously
 * disabled and wakes all threads currently blocked waiting to send.
 *
 * @param ch Channel to modify
 * @param d Direction to enable (owner_to_workers or workers_to_owner)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch is NULL or d is invalid
 *
 * @note Wakes all blocked senders via broadcast
 * @note Threads can immediately attempt to send if space is available
 * @note Does not affect the opposite direction
 *
 * @see chan_disable_sending
 */
ccol_retval_t chan_enable_sending(channel *ch, channel_direction d);

/**
 * @brief Get current message count for a specific channel direction
 *
 * Returns the number of messages currently stored in the specified direction's
 * queue.
 *
 * @param ch Channel to query
 * @param d Direction to query (owner_to_workers or workers_to_owner)
 *
 * @return Number of messages in the specified direction, or (size_t)-1 on error
 *
 * @note Thread-safe snapshot of message count
 * @note Return value of (size_t)-1 indicates error (NULL channel or invalid
 * direction)
 */
size_t chan_msg_count(channel *ch, channel_direction d);