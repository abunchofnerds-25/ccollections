/*
MIT License

Copyright (c) 2026 - A bunch of nerds

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
  void *data; /**< Pointer to message data (ownership is transferred on send) */
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
  circular_queue_create_with_mprocs((max_size), NULL, (err_str))

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
 * Frees all resources associated with the queue. If there are any messages
 * still in the queue, the function __circular_queue_destroy which is called
 * by this macro will assert - drain the queue before destroying.
 *
 * @param cq Circular queue to destroy (will be set to NULL after destruction)
 *
 * @warning Messages remaining in the queue are not automatically freed
 * @note Safe to call with NULL pointer
 */
#define circular_queue_destroy(cq)  \
  do {                              \
    __circular_queue_destroy((cq)); \
    cq = NULL;                      \
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
 * @note Remains blocked even when sending is temporarily disabled
 * @note Caller receives ownership of target_buf->data and must free it
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
  dynamic_queue_create_with_mprocs(NULL, (err_str))

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
 * nodes. If there are any messages still in the queue, the function
 * __dynamic_queue_destroy which is called by this macro will assert - drain
 * the queue before destroying.
 *
 * @param dq Dynamic queue to destroy (will be set to NULL after destruction)
 *
 * @warning Messages remaining in the queue are not automatically freed
 * @note Safe to call with NULL pointer
 */
#define dynamic_queue_destroy(dq)  \
  do {                             \
    __dynamic_queue_destroy((dq)); \
    dq = NULL;                     \
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
 * @note Remains blocked even when sending is temporarily disabled
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
  channel_create_with_mprocs((max_size), NULL, (err_str))

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
 * circular queues. If there are any messages still in any of the underlying
 * queues, the function __channel_destroy which is called by this macro will
 * assert - drain the both directions before destroying.
 *
 * @param ch Channel to destroy (will be set to NULL after destruction)
 *
 * @warning Messages remaining in either direction are not automatically freed
 * @note Safe to call with NULL pointer
 */
#define channel_destroy(ch)  \
  do {                       \
    __channel_destroy((ch)); \
    ch = NULL;               \
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

/* ========================================================================== */
/*                             CCOL_SELECT API */
/* ========================================================================== */

/**
 * @brief Selectable type tag for ccol_select
 */
typedef enum {
  ccol_selectable_circq, /**< Wraps a circular_queue pointer */
  ccol_selectable_dynq,  /**< Wraps a dynamic_queue pointer  */
  ccol_selectable_fd,    /**< Wraps a raw file descriptor     */
} ccol_selectable_type;

/**
 * @brief Direction of interest for a ccol_selectable
 *
 * ccol_select_read;  wait until the queue has at least one message to receive.
 * ccol_select_write; wait until the queue has room to accept at least one send
 *                   (circular_queue: msg_count < max_size && !writing_disabled;
 *                    dynamic_queue: !writing_disabled).
 *
 * Neither direction is consumed or reserved by ccol_select() itself; the
 * caller must call circq_try_recv_zc/dynmq_try_recv_zc (read-direction win)
 * or circq_try_send_zc/dynmq_try_send_zc (write-direction win) after waking.
 * A TOCTOU race is possible (same as POSIX select(2)), so that call must
 * be non-blocking.
 */
typedef enum {
  ccol_select_read,  /**< Wait for at least one readable message      */
  ccol_select_write, /**< Wait for space to send at least one message */
} ccol_select_dir;

/**
 * @brief Tagged union representing a single queue to be watched by ccol_select
 *
 * Construct with selectable_from_circq(q, dir), selectable_from_dynq(q, dir),
 * or selectable_from_chan(ch, dir).  Pass ccol_select_read to wait for a
 * message to arrive; pass ccol_select_write to wait until the queue has room.
 * For channels the correct underlying queue is resolved at construction time
 * from the calling thread's ID, exactly as chan_recv_zc() / chan_send_zc() do.
 *
 * ccol_select() never performs the receive/send itself for any selectable
 * type; it only reports readiness (see ccol_select()'s own documentation).
 */
typedef struct {
  ccol_selectable_type type;
  ccol_select_dir dir;
  union {
    circular_queue *cq;
    dynamic_queue *dq;
    int fd; /**< Used when type == ccol_selectable_fd; must be >= 0 */
  };
} ccol_selectable;

/** @brief Build a selectable from a circular_queue pointer
 *
 *  @param q_       circular_queue pointer to watch
 *  @param sel_dir  ccol_select_read or ccol_select_write
 */
#define selectable_from_circq(q_, sel_dir) \
  ((ccol_selectable){                      \
      .type = ccol_selectable_circq, .dir = (sel_dir), .cq = (q_)})

/** @brief Build a selectable from a dynamic_queue pointer
 *
 *  @param q_       dynamic_queue pointer to watch
 *  @param sel_dir  ccol_select_read or ccol_select_write
 */
#define selectable_from_dynq(q_, sel_dir) \
  ((ccol_selectable){                     \
      .type = ccol_selectable_dynq, .dir = (sel_dir), .dq = (q_)})

/** @brief Build a selectable from a raw file descriptor
 *
 *  ccol_select uses epoll(7) internally whenever any fd selectable is present.
 *  Queue selectables in the same array are bridged via per-waiter eventfd(2)s
 *  so both fd and queue readiness are multiplexed on a single epoll_wait call.
 *
 *  ccol_select() never reads or writes the fd itself; it only reports
 *  readiness (see ccol_select()'s own documentation). The caller performs
 *  its own read(2)/recv(2) or write(2)/send(2) on fd_ afterward.
 *
 *  @param fd_      File descriptor to watch (must be >= 0)
 *  @param sel_dir  ccol_select_read  -> EPOLLIN (readable)
 *                  ccol_select_write -> EPOLLOUT (writable)
 *
 *  @note EPOLLRDHUP, EPOLLERR, and EPOLLHUP are always included for read
 *        selectables; EPOLLERR and EPOLLHUP for write selectables.
 */
#define selectable_from_fd(fd_, sel_dir) \
  ((ccol_selectable){.type = ccol_selectable_fd, .dir = (sel_dir), .fd = (fd_)})

/**
 * @brief Resolve a channel's queue for the calling thread and return a
 *        ccol_selectable for the requested direction
 *
 * The direction determines both which queue is selected and the waiter list
 * used inside ccol_select():
 *
 *   ccol_select_read;  owner reads from workers_to_owner_cq; worker reads
 *                     from owner_to_workers_cq  (same as chan_recv_zc)
 *   ccol_select_write; owner writes to owner_to_workers_cq; worker writes
 *                     to workers_to_owner_cq    (same as chan_send_zc)
 *
 * As with every other queue selectable, readiness on the resolved queue is
 * receive-explicit: ccol_select() only reports that the queue is ready, the
 * caller performs its own chan_try_recv_zc()/chan_try_send_zc() (or the
 * equivalent circq_try_recv_zc()/circq_try_send_zc() on the resolved queue)
 * afterward.
 *
 * @param ch  Channel to resolve (NULL yields a selectable ccol_select()
 * rejects)
 * @param dir ccol_select_read or ccol_select_write
 * @return ccol_selectable wrapping the direction-resolved circular_queue
 */
ccol_selectable ccol_selectable_from_chan(channel *ch, ccol_select_dir dir);

/** @brief Build a selectable from a channel, resolving the queue for this
 * thread
 *
 *  @param ch_      channel pointer
 *  @param sel_dir  ccol_select_read or ccol_select_write
 */
#define selectable_from_chan(ch_, sel_dir) \
  ccol_selectable_from_chan((ch_), (sel_dir))

/**
 * @brief Wait for readability or writability on any of n selectables
 *
 * Blocks until at least one selectable is ready according to its direction,
 * then sets *ready_index and returns. ccol_select() never performs the
 * receive or send itself, for any selectable type (queue or fd): the caller
 * must, immediately afterward, perform its own explicit receive/send on the
 * winning selectable: circq_try_recv_zc()/dynmq_try_recv_zc() or
 * circq_try_send_zc()/dynmq_try_send_zc() for a queue selectable,
 * read(2)/recv(2) or write(2)/send(2) on selectables[*ready_index].fd for an
 * fd selectable. This is a TOCTOU-safe contract (the same one write-direction
 * wins have always had): a concurrent consumer/producer may win the race
 * between ccol_select() returning and the caller's own explicit call, so
 * that call must be non-blocking and its result checked.
 *
 * Mixed read+write arrays are supported: any selectable in the array may have
 * any direction.  The first one that becomes ready wins the race.
 *
 * Waiter nodes are heap-allocated (one per selectable).  Any number of threads
 * may simultaneously call ccol_select watching the same queue; there is no
 * fixed cap on concurrent waiters.
 *
 * Equivalent to ccol_select_timed(ready_index, n, selectables, -1).
 *
 * @param ready_index Set to the index of the selectable that became ready
 *                    (valid only when ccol_success is returned)
 * @param n           Number of selectables (must be >= 1)
 * @param selectables Array of n ccol_selectable values to monitor
 *
 * @return ccol_success           A selectable is ready; *ready_index is set.
 *                                The caller must perform its own explicit
 *                                receive/send afterward (see above).
 * @return ccol_invalid_args      Any argument is NULL/zero; a queue selectable
 *                                contains a NULL queue pointer; an fd
 * selectable has fd < 0; or an unknown type/dir value
 * @return ccol_not_enough_memory Internal waiter-node allocation (one node per
 *                                selectable) failed; no state was changed
 * @return ccol_unexpected_failure epoll_create1, eventfd, or epoll_ctl failed
 *
 * @note When no fd selectables are present, only POSIX condition variables are
 *       used; no additional system calls occur beyond normal queue operations
 * @note When fd selectables are present, epoll(7) and eventfd(2) are used
 *       internally (Linux-only)
 */
ccol_retval_t ccol_select(size_t *ready_index, size_t n,
                          ccol_selectable *selectables);

/**
 * @brief Wait for readability or writability on any of n selectables with
 * timeout
 *
 * Identical to ccol_select() except that the call returns ccol_timed_out if no
 * selectable becomes ready within timeout_ms milliseconds.
 *
 * @param ready_index Set to the index of the selectable that became ready.
 *                    Valid when ccol_success is returned.
 *                    Unmodified on ccol_timed_out, ccol_invalid_args,
 *                    ccol_not_enough_memory, or ccol_unexpected_failure.
 * @param n           Number of selectables (must be >= 1)
 * @param selectables Array of n ccol_selectable values to monitor
 * @param timeout_ms  Maximum time to wait in milliseconds.  Pass -1 for an
 *                    infinite wait (same behaviour as ccol_select).  Pass 0
 *                    to poll without blocking.
 *
 * @return ccol_success           A selectable is ready; *ready_index is set.
 * @return ccol_timed_out         timeout_ms elapsed without any selectable
 *                                becoming ready; *ready_index is unmodified.
 * @return ccol_invalid_args      (same conditions as ccol_select)
 * @return ccol_not_enough_memory (same conditions as ccol_select)
 * @return ccol_unexpected_failure (same conditions as ccol_select)
 *
 * @note Uses CLOCK_MONOTONIC for the deadline so system time adjustments do
 *       not affect the timeout.
 * @note When no fd selectables are present, pthread_cond_timedwait is used
 *       on a CLOCK_MONOTONIC condvar; when fd selectables are present, the
 *       remaining time is passed to each epoll_wait call.
 */
ccol_retval_t ccol_select_timed(size_t *ready_index, size_t n,
                                ccol_selectable *selectables, int timeout_ms);

/**
 * @brief Convenience macro: call ccol_select with inline selectable list
 *
 * Builds the selectable array from the variadic arguments, computes the count
 * at compile time, and forwards everything to ccol_select.  The result is the
 * ccol_retval_t returned by ccol_select.
 *
 * Each argument must be a ccol_selectable value (typically produced by
 * selectable_from_circq(), selectable_from_dynq(), or
 * selectable_from_chan() with a ccol_select_read or ccol_select_write dir).
 * All arguments are evaluated exactly once.
 *
 * Example:
 * @code
 *   size_t idx;
 *   ccol_retval_t r = ccol_select_va(&idx,
 *       selectable_from_circq(q0, ccol_select_read),
 *       selectable_from_chan(ch, ccol_select_read));
 *   if (r == ccol_success) {
 *       // perform the explicit receive appropriate to whichever
 *       // selectable won, e.g. circq_try_recv_zc(q0, &msg)
 *   }
 * @endcode
 *
 * @note Uses a GCC/Clang statement expression; not valid under strict ISO C
 */
#define ccol_select_va(ready_index, ...)                                   \
  __extension__({                                                          \
    ccol_selectable _cqsel_arr[] = {__VA_ARGS__};                          \
    ccol_select((ready_index), sizeof(_cqsel_arr) / sizeof(_cqsel_arr[0]), \
                _cqsel_arr);                                               \
  })

/**
 * @brief Convenience macro: call ccol_select_timed with inline selectable list
 *
 * Identical to ccol_select_va() but passes timeout_ms to ccol_select_timed.
 *
 * Example:
 * @code
 *   size_t idx;
 *   ccol_retval_t r = ccol_select_timed_va(&idx, 500,
 *       selectable_from_circq(q0, ccol_select_read),
 *       selectable_from_chan(ch, ccol_select_read));
 * @endcode
 *
 * @note Uses a GCC/Clang statement expression; not valid under strict ISO C
 */
#define ccol_select_timed_va(ready_index, timeout_ms, ...)                    \
  __extension__({                                                             \
    ccol_selectable _cqsel_arr[] = {__VA_ARGS__};                             \
    ccol_select_timed((ready_index),                                          \
                      sizeof(_cqsel_arr) / sizeof(_cqsel_arr[0]), _cqsel_arr, \
                      (timeout_ms));                                          \
  })

/* ========================================================================== */
/*                             EVENT_LOOP API                                 */
/* ========================================================================== */

/**
 * @brief Opaque event_loop structure
 *
 * A persistent, incrementally-mutable epoll(7)-based reactor. Exactly ONE
 * dedicated poller thread ever calls epoll_wait, for every configuration;
 * this is a deliberate design property, not an implementation detail: with
 * more than one thread independently calling epoll_wait on a shared epoll
 * instance (an earlier design this module used), a single ready event wakes
 * every blocked thread (a genuine kernel-level thundering herd, confirmed
 * against real epoll(7) behavior; the non-obvious part is that
 * EPOLLEXCLUSIVE does NOT help here; it governs the same target fd
 * registered across multiple SEPARATE epoll instances, not many threads
 * sharing one), which measurably hurt tail latency for low-concurrency
 * workloads. See num_reactor_threads on event_loop_create_with_mprocs for
 * how multi-threaded DISPATCH throughput is still provided despite only one
 * thread ever polling.
 *
 * Registrations are built from the same ccol_selectable type ccol_select
 * uses (selectable_from_fd, selectable_from_circq, selectable_from_dynq,
 * selectable_from_chan). Like ccol_select, event_loop never performs the
 * receive or send itself for any selectable type: the caller always
 * performs its own read()/recv()/circq_try_recv_zc()/dynmq_try_recv_zc()
 * from inside the callback (see event_loop_add's documentation).
 *
 * With num_reactor_threads > 1 (callbacks running on separate worker
 * threads, not the poller), two correctness properties hold that a
 * single-threaded reactor gets for free from having only one caller:
 *  - A single registration's callback(s) are never invoked concurrently with
 *    themselves, and (stricter than a bare "no self-concurrency" guarantee)
 *    a read registration and a write registration sharing the same fd
 *    never run concurrently with *each other* either, so a callback pair
 *    that shares state across both directions on one fd (e.g. one TLS
 *    connection object) needs no additional locking of its own.
 *  - Removing a registration and reusing its underlying fd number for an
 *    unrelated new registration is safe even while a worker thread may
 *    still be mid-processing a dispatch that referenced the old
 *    registration: dispatch always validates liveness immediately before
 *    invoking a callback, so a stale reference to an already-removed
 *    registration is a safe no-op, never a use-after-free or a misdirected
 *    callback on the new registration. A second registration for the same
 *    entry is never collected while an earlier one is still in flight
 *    (queued or executing) either, for the identical reason; application
 *    code that calls event_loop_modify from within an in-flight callback
 *    (a supported, commonly-used pattern) does not race a second,
 *    concurrently-collected dispatch for that same registration.
 * See event_loop_reg_generation() for the caller-visible identity token this
 * makes available for a registration's own defensive bookkeeping across fd
 * reuse; a separate, additional concern from the two guarantees above,
 * which hold unconditionally whether or not a caller ever inspects it.
 */
typedef struct event_loop_s event_loop_s;

/** @brief Handle type (pointer to opaque struct) */
typedef event_loop_s *event_loop;

/** @brief Opaque handle to a single event_loop registration */
typedef struct event_reg event_reg;

/**
 * @brief Callback invoked when a registration becomes readable
 *
 * Reports readiness only, for every selectable type: the reactor never
 * performs the receive itself. For fd selectables, the callback performs
 * its own read()/recv() on sel->fd. For queue selectables (circq/dynq/
 * chan-resolved), the callback performs its own circq_try_recv_zc()/
 * dynmq_try_recv_zc() on sel->cq/sel->dq. Either way a concurrent consumer
 * may have already claimed the data (a TOCTOU race inherent to the design,
 * same as ccol_select's own write-direction contract); the callback's own
 * explicit receive call may find nothing, and must handle that gracefully.
 *
 * @param loop The event_loop this registration belongs to
 * @param sel  The ccol_selectable this registration was created from
 *             (sel->dir reflects the registration's current direction,
 *             which may have changed since event_loop_add via
 *             event_loop_modify)
 * @param arg  The opaque pointer passed to event_loop_add
 */
typedef void (*event_readable_fn)(event_loop loop, ccol_selectable *sel,
                                  void *arg);

/**
 * @brief Callback invoked when a registration becomes writable
 *
 * No payload is ever delivered: for fd selectables the caller performs its
 * own write()/send(); for queue selectables the registration only signals
 * that room may be available; the callback must call
 * circq_try_send_zc/dynmq_try_send_zc itself (a TOCTOU race is possible,
 * same contract as ccol_select's write-direction wins).
 *
 * @param loop The event_loop this registration belongs to
 * @param sel  The ccol_selectable this registration was created from
 * @param arg  The opaque pointer passed to event_loop_add
 */
typedef void (*event_writable_fn)(event_loop loop, ccol_selectable *sel,
                                  void *arg);

/**
 * @brief Callback invoked on a fatal condition for a registration
 *
 * For fd selectables, fires on EPOLLERR/EPOLLHUP (the fd itself is broken;
 * if both directions are registered on the same fd, both receive an error
 * dispatch). Queue selectables never produce an error condition and never
 * invoke this callback.
 *
 * @param loop The event_loop this registration belongs to
 * @param sel  The ccol_selectable this registration was created from
 * @param arg  The opaque pointer passed to event_loop_add
 */
typedef void (*event_error_fn)(event_loop loop, ccol_selectable *sel,
                               void *arg);

/**
 * @brief Callback bundle for a single event_loop_add call
 *
 * Any of the three may be NULL, in which case that class of event is
 * silently dropped for this registration (e.g. a write-only producer that
 * never expects on_error may pass NULL there).
 */
typedef struct event_handlers {
  event_readable_fn on_readable; /**< EPOLLIN|EPOLLRDHUP, or a queue message */
  event_writable_fn on_writable; /**< EPOLLOUT, or queue room available */
  event_error_fn on_error;       /**< EPOLLERR|EPOLLHUP (fd selectables only) */
} event_handlers_t;

/**
 * @brief Create an event_loop with custom memory management
 *
 * Creates a persistent epoll instance and immediately spawns the threads
 * that drive it (every thread is ready to dispatch events as soon as this
 * call returns, mirroring create_cthread_pool's "ready to work the moment
 * you get the handle" ergonomics).
 *
 * num_reactor_threads == 1 reproduces this module's original single-thread
 * design exactly, byte-for-byte: that one thread both calls epoll_wait AND
 * runs every callback inline. num_reactor_threads > 1 spawns exactly ONE
 * dedicated thread that calls epoll_wait (never more; see the event_loop
 * struct's own doc comment for why) plus (num_reactor_threads - 1) worker
 * threads that actually execute callbacks, so total OS thread count for a
 * given num_reactor_threads is always exactly that value, preserving the
 * parameter's resource-usage meaning across both configurations. A
 * registration's callback runs on the poller thread for the first
 * configuration, or on one of the worker threads for the second; this is
 * transparent to callback code (event_readable_fn/event_writable_fn/
 * event_error_fn have no way to observe which), except that
 * event_loop_shutdown must not be called from within a callback running on
 * EITHER kind of thread (self-join hazard; see event_loop_shutdown's own
 * doc comment).
 *
 * Benchmarked, not assumed (per this project's standing performance-
 * regression-is-a-bug rule): num_reactor_threads == 1 measures byte-for-
 * byte identical to the pre-poller-split single-thread design, as expected
 * from the unchanged code path. With more than one thread, this design
 * closes a real, measured tail-latency regression the original "N threads
 * all call epoll_wait" design had at low concurrency (a genuine kernel
 * thundering herd, not specific to this module's own code; see the
 * event_loop struct's own doc comment) while preserving aggregate
 * multi-threaded dispatch throughput under real concurrent load.
 *
 * The fd/entry registry is lock-striped: num_lock_stripes independent
 * (mutex, chmap) pairs, each guarding a disjoint subset of registrations
 * (one real fd, or one queue/channel registration's private bridge fd, is
 * always handled by exactly one stripe; never split across two). Passing
 * 1 reproduces the original single-lock design exactly, just with one
 * extra array indirection; passing more lets event_loop_add/_remove/_modify
 * calls for different fds/registrations proceed concurrently instead of
 * serializing through one lock, at the cost of num_lock_stripes mutexes and
 * chmaps being allocated up front. This is independent of
 * num_reactor_threads: the stripe count controls registry contention, the
 * thread count controls dispatch concurrency.
 *
 * @param max_events_per_wait Size of the epoll_wait batch buffer the poller
 * thread uses (must be >= 1); bounds how many ready events a single
 * epoll_wait call drains, not the number of registrations the loop can hold
 * @param num_lock_stripes Number of independent lock stripes for the fd/
 * entry registry (must be >= 1). 1 matches this module's original
 * single-lock behavior; pass a larger value to reduce
 * event_loop_add/_remove/_modify contention across many different fds/
 * registrations under concurrent use. No upper bound is enforced.
 * @param num_reactor_threads Total OS thread count devoted to this loop's
 * own polling and dispatch (must be >= 1). 1 matches this module's original
 * single-thread behavior exactly (one thread polls and dispatches inline);
 * any larger value means exactly one dedicated polling thread plus
 * (num_reactor_threads - 1) dispatch worker threads. See the event_loop
 * struct's own doc comment for the two correctness guarantees that hold
 * regardless of this value.
 * @param mmgmt_procs Custom memory management procedures, or NULL to use
 * default malloc/free
 * @param err_str Optional pointer to receive error string on failure (pass
 * NULL to ignore)
 *
 * @return New event_loop handle, or NULL on failure (invalid arguments,
 * allocation failure, epoll_create1/eventfd/pthread_create failure)
 *
 * @see event_loop_create
 * @see event_loop_destroy
 */
event_loop event_loop_create_with_mprocs(size_t max_events_per_wait,
                                         size_t num_lock_stripes,
                                         size_t num_reactor_threads,
                                         ccol_memmgmt_procs_t *mmgmt_procs,
                                         char **err_str);

/**
 * @brief Create an event_loop with default memory management
 *
 * Convenience macro equivalent to event_loop_create_with_mprocs with
 * mmgmt_procs = NULL.
 */
#define event_loop_create(max_events_per_wait, num_lock_stripes,           \
                          num_reactor_threads, err_str)                    \
  event_loop_create_with_mprocs((max_events_per_wait), (num_lock_stripes), \
                                (num_reactor_threads), NULL, (err_str))

/**
 * @brief Register a selectable with the event loop
 *
 * Builds on the same ccol_selectable type ccol_select uses:
 * selectable_from_fd, selectable_from_circq, selectable_from_dynq, and
 * selectable_from_chan are all directly reusable to build sel.
 *
 * One registration covers exactly one direction (sel.dir). A caller wanting
 * both directions live on the same fd at once (e.g. a full-duplex pipe)
 * calls event_loop_add twice and gets two independent handles; flipping a
 * single registration's direction over time (e.g. a connecting socket:
 * write-interest until connect completes, then read-interest afterward)
 * uses one registration plus event_loop_modify instead. Registering a
 * second selectable for a direction already occupied on the same fd (two
 * read registrations on one fd, for example) is rejected with
 * ccol_not_permitted.
 *
 * @param loop     event_loop to register with
 * @param sel      What to watch (see above)
 * @param handlers Callback bundle (individual callbacks may be NULL)
 * @param arg      Opaque pointer passed to every callback for this
 *                 registration
 * @param err_str  Optional pointer to receive error string on failure
 *
 * @return New registration handle, or NULL on failure
 *
 * @note Thread-safe; may be called concurrently with event_loop_remove,
 *       event_loop_modify, and from within a callback running on the
 *       reactor thread
 *
 * @see event_loop_remove
 * @see event_loop_modify
 */
event_reg *event_loop_add(event_loop loop, ccol_selectable sel,
                          event_handlers_t handlers, void *arg, char **err_str);

/**
 * @brief Caller-visible identity token for a registration's underlying fd
 *
 * A monotonically increasing value, unique loop-wide, minted once when the
 * fd (or queue/channel bridge) reg belongs to is first registered
 * (event_loop_add's new-entry path) and shared by every registration on
 * that same fd for as long as it lives, including across
 * event_loop_modify (a direction flip is the same underlying fd/connection,
 * so it keeps the same generation) and across both a read and a write
 * registration on the same fd (both share one generation, since they
 * represent one logical connection).
 *
 * This exists for a caller's own defensive bookkeeping across fd reuse: a
 * caller holding onto a connection object across several async steps can
 * stamp it with the generation it read right after event_loop_add returned,
 * and later compare against a fresh read to detect whether it's still
 * reasoning about the same logical connection. It is not required for basic
 * correctness; event_loop's own dispatch already validates a registration's
 * liveness before invoking any callback, unconditionally, whether or not a
 * caller ever calls this function at all (see the event_loop struct's own
 * doc comment).
 *
 * Safe to call even on an already-removed reg: like event_loop_reg_count's
 * sibling accessors, this reads a field set once at registration and never
 * written again, protected by reg's own deferred-free contract rather than
 * requiring reg to still be live.
 *
 * @param reg Registration to query (must not be NULL)
 * @return The generation value, or 0 if reg is NULL (0 is never a valid
 * generation for a real registration, since the counter starts at 1)
 */
uint64_t event_loop_reg_generation(const event_reg *reg);

/**
 * @brief Change an existing fd registration's direction
 *
 * fd-only: called on a registration built from a queue/channel selectable,
 * returns ccol_invalid_args (a queue selectable's direction is part of its
 * identity; remove and re-add instead). On success, updates the
 * registration's ccol_selectable.dir (visible to subsequent callbacks via
 * the sel parameter), moves it to the other slot on its underlying fd, and
 * recomputes the fd's combined epoll interest mask, without a window
 * where the fd is briefly unregistered.
 *
 * @param loop    event_loop the registration belongs to
 * @param reg     Registration to modify
 * @param new_dir ccol_select_read or ccol_select_write
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop/reg is NULL, reg is a queue/channel
 * registration, new_dir is invalid, or reg was concurrently removed
 * @return ccol_not_permitted if the target direction is already occupied by
 * a different registration on the same fd
 *
 * @note Thread-safe; may be called concurrently with event_loop_remove and
 *       from within a callback running on the reactor thread
 */
ccol_retval_t event_loop_modify(event_loop loop, event_reg *reg,
                                ccol_select_dir new_dir);

/**
 * @brief Temporarily stop delivering events for an fd registration, without
 *        destroying it
 *
 * fd-only, same restriction as event_loop_modify. Unlike event_loop_remove
 * (which fully unregisters reg and defers it for freeing), event_loop_pause
 * leaves reg fully intact (still occupying its slot on the underlying
 * fd's entry, still counting toward event_loop_reg_count, still carrying
 * the same event_loop_reg_generation) and only recomputes the fd's
 * combined epoll interest mask to exclude it. No on_readable/on_writable/
 * on_error callback fires for reg while paused, exactly as if it had been
 * removed; the other direction on the same fd (if any) is unaffected.
 *
 * This is the cheap alternative to an event_loop_remove immediately
 * followed by a later event_loop_add for a caller pattern where the same
 * logical registration is going to come back; e.g. a connection handed
 * off to a worker thread for blocking body I/O, then handed back to the
 * reactor for its next request: no heap allocation/free, no fd-registry
 * chmap churn, and (with a single reactor thread, i.e.
 * num_reactor_threads == 1) one epoll_ctl call instead of the two (DEL,
 * then ADD) a remove-then-add pair costs. With more than one reactor
 * thread, the dispatch worker's own post-callback EPOLLONESHOT re-arm
 * still runs once more after a paused callback returns (harmless:
 * event_loop's internal re-arm helper always recomputes the mask fresh
 * from live state, so a redundant re-arm reapplies the same excluding mask
 * rather than reintroducing a race) but pause/resume still avoids the
 * allocation and registry churn in that configuration too.
 *
 * Pausing an already-paused reg is a no-op success.
 *
 * @param loop event_loop the registration belongs to
 * @param reg  Registration to pause
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop/reg is NULL, reg is a queue/channel
 * registration, or reg was concurrently removed
 *
 * @note Thread-safe; may be called concurrently with event_loop_remove and
 *       from within a callback running on the reactor thread
 *
 * @see event_loop_resume
 * @see event_loop_remove
 */
ccol_retval_t event_loop_pause(event_loop loop, event_reg *reg);

/**
 * @brief Resume event delivery for a registration previously paused by
 *        event_loop_pause
 *
 * Recomputes the fd's combined epoll interest mask to include reg again.
 * Resuming a reg that is not currently paused (never paused, or already
 * resumed) is a no-op success. This deliberately mirrors
 * event_loop_modify's own "already in the requested state" idempotence.
 *
 * @param loop event_loop the registration belongs to
 * @param reg  Registration to resume
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop/reg is NULL, reg is a queue/channel
 * registration, or reg was concurrently removed (e.g. the connection was
 * closed while the caller still thought it owned a paused registration to
 * resume)
 *
 * @note Thread-safe; may be called concurrently with event_loop_remove and
 *       from within a callback running on the reactor thread
 *
 * @see event_loop_pause
 */
ccol_retval_t event_loop_resume(event_loop loop, event_reg *reg);

/**
 * @brief Deregister a selectable from the event loop
 *
 * Safe to call from within a callback for the very registration being
 * removed (self-removal on error is a common pattern) as well as from any
 * other thread, including concurrently with an in-flight dispatch for the
 * same registration; teardown is deferred until any in-progress callback
 * returns. No further callback for this registration is ever invoked after
 * this call returns, even one already collected (e.g. sitting queued for a
 * dispatch worker thread with num_reactor_threads > 1) but not yet actually
 * started; callers may free whatever the registration's own arg points to
 * immediately after this call returns without racing a stale callback
 * invocation.
 *
 * Does not close an fd or affect a queue's own lifetime; only the
 * event_loop's registration bookkeeping is released, mirroring
 * selectable_from_fd's existing "caller owns the fd" contract.
 *
 * @param loop event_loop the registration belongs to
 * @param reg  Registration to remove
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop or reg is NULL
 *
 * @note Thread-safe
 */
ccol_retval_t event_loop_remove(event_loop loop, event_reg *reg);

/**
 * @brief Number of currently-registered event_reg handles
 *
 * Counts live registrations (event_loop_add calls not yet removed), not
 * epoll interest-list entries; one fd with both directions registered
 * counts as 2.
 *
 * @param loop event_loop to query
 * @return Registration count, or (size_t)-1 if loop is NULL
 */
size_t event_loop_reg_count(event_loop loop);

/**
 * @brief Stop the reactor's poller thread and dispatch worker threads (if
 *        any)
 *
 * Idempotent: safe to call more than once, or not at all before
 * event_loop_destroy (which calls this internally if needed). Blocks until
 * the poller thread has been joined and, for num_reactor_threads > 1, until
 * every dispatch worker has finished its current job and been joined too
 * (a graceful drain, not a cancel: no dispatch can be in flight once this
 * returns, matching num_reactor_threads == 1's own guarantee).
 *
 * @param loop event_loop to shut down
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop is NULL
 * @return ccol_not_permitted if called from within a callback running on
 * any of this loop's own threads (the poller, or, for num_reactor_threads
 * > 1, a dispatch worker); see the warning below
 *
 * @warning Calling this from within a callback running on one of this
 * loop's own threads would otherwise join that thread from itself
 * (undefined behavior / EDEADLK); detected and rejected with
 * ccol_not_permitted rather than left as caller-triggerable undefined
 * behavior. Defer shutdown to another thread, or to after the callback
 * returns, instead.
 */
ccol_retval_t event_loop_shutdown(event_loop loop);

/**
 * @brief Destroy an event_loop (internal function)
 *
 * @param loop event_loop to destroy
 *
 * @warning Do not call directly - use event_loop_destroy() macro instead
 */
void __event_loop_destroy(event_loop loop);

/**
 * @brief RAII cleanup function (used with _ccol_destructor)
 */
static inline __attribute__((always_inline)) void ___event_loop_destroy(
    event_loop *lp) {
  if (lp && *lp) {
    __event_loop_destroy(*lp);
    *lp = NULL;
  }
}

/**
 * @brief Destroy an event_loop and set handle to NULL
 *
 * Shuts the reactor thread down (if not already shut down) and frees every
 * remaining registration; for queue-backed registrations this correctly
 * unlinks each one from its queue's own waiter list first, so a queue that
 * outlives this event_loop is never left with a dangling waiter pointer.
 *
 * @param loop event_loop to destroy (will be set to NULL after destruction)
 *
 * @note Safe to call with NULL pointer
 */
#define event_loop_destroy(loop)  \
  do {                            \
    __event_loop_destroy((loop)); \
    (loop) = NULL;                \
  } while (0)

/**
 * @brief Declare an uninitialised event_loop variable
 *
 * Must be followed by event_loop_construct or an event_loop_create* call.
 */
#define event_loop_declare(name) event_loop name

/**
 * @brief Declare with automatic destruction on scope exit
 */
#define event_loop_declare_scoped(name) \
  event_loop name _ccol_destructor(___event_loop_destroy) = NULL

/**
 * @brief Declare and initialise in one step; fatal_err on failure
 *
 * Example:
 * @code
 * event_loop_construct(loop, 32, 1, 1);
 * event_reg *r = event_loop_add(loop, selectable_from_fd(fd, ccol_select_read),
 *                                handlers, NULL, NULL);
 * event_loop_destroy(loop);
 * @endcode
 *
 * @param name               Variable name for the event_loop handle
 * @param max_events_per_wait Size of the epoll_wait batch buffer (must be
 *                            >= 1)
 * @param num_lock_stripes   Number of lock stripes for the fd/entry
 *                           registry (must be >= 1; 1 matches the original
 *                           single-lock behavior)
 * @param num_reactor_threads Number of background reactor threads (must be
 *                           >= 1; 1 matches the original single-thread
 *                           behavior)
 */
#define event_loop_construct(name, max_events_per_wait, num_lock_stripes, \
                             num_reactor_threads)                         \
  event_loop name = NULL;                                                 \
  do {                                                                    \
    char *_evl_err = NULL;                                                \
    (name) = event_loop_create((max_events_per_wait), (num_lock_stripes), \
                               (num_reactor_threads), &_evl_err);         \
    if (!(name)) {                                                        \
      fatal_err("event_loop_construct('%s'): %s", #name,                  \
                _evl_err ? _evl_err : "unknown error");                   \
    }                                                                     \
  } while (0)

/**
 * @brief Declare, initialise, and auto-destroy on scope exit; fatal_err on
 *        failure
 *
 * @param name               Variable name for the event_loop handle
 * @param max_events_per_wait Size of the epoll_wait batch buffer (must be
 *                            >= 1)
 * @param num_lock_stripes   Number of lock stripes for the fd/entry
 *                           registry (must be >= 1; 1 matches the original
 *                           single-lock behavior)
 * @param num_reactor_threads Number of background reactor threads (must be
 *                           >= 1; 1 matches the original single-thread
 *                           behavior)
 */
#define event_loop_construct_scoped(name, max_events_per_wait,             \
                                    num_lock_stripes, num_reactor_threads) \
  event_loop name _ccol_destructor(___event_loop_destroy) = NULL;          \
  do {                                                                     \
    char *_evl_err = NULL;                                                 \
    (name) = event_loop_create((max_events_per_wait), (num_lock_stripes),  \
                               (num_reactor_threads), &_evl_err);          \
    if (!(name)) {                                                         \
      fatal_err("event_loop_construct_scoped('%s'): %s", #name,            \
                _evl_err ? _evl_err : "unknown error");                    \
    }                                                                      \
  } while (0)

/* ========================================================================== */
/*                         UNIT TEST INTERNALS                                */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Expose the number of dispatch jobs currently queued or executing
 *        on loop's own dispatch_pool, for testing
 *
 * Returns 0 for a NULL loop or a loop constructed with num_reactor_threads
 * == 1 (no dispatch_pool exists in that configuration). The direct
 * regression check for the EPOLLONESHOT re-arm mechanism: a continuously-
 * ready fd or queue selectable must not cause this count to grow without
 * bound while dispatch_pool's own workers are still catching up.
 *
 * @param loop  event_loop to query
 * @return      Number of pending/in-flight dispatch jobs
 */
size_t event_loop_dispatch_pool_pending_count_for_tests(event_loop loop);
#endif
