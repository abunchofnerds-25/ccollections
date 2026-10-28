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

#include "common.h"

/* Everything this header declares from here to the end of the file belongs
 * to the public Application Binary Interface (ABI) of libccollections, and
 * the shared library exports all of it. The library itself is built with
 * -fvisibility=hidden, so a function or object outside these blocks stays
 * internal: it isn't in the dynamic symbol table, an application linking
 * against the library can't interpose it, and a symbol of the same name in
 * that application can't collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cthreadcomm.h
 * @brief Thread-safe message passing primitives for communication between
 * threads
 *
 * This header has three thread-safe ways to pass messages:
 * - ccol_circular_queue: a fixed-size queue whose send blocks while it is
 *   full, which is how it applies backpressure.
 * - ccol_dynamic_queue: a queue with no size limit (a linked list) whose
 *   send never blocks.
 * - ccol_channel: two-way communication between an owner thread and a worker
 *   thread.
 *
 * Every operation is zero-copy, it hands over the ownership of a pointer
 * instead of copying the data.
 */

/** @brief Opaque handle to a circular queue */
typedef struct ccol_circular_queue ccol_circular_queue;

/** @brief Opaque handle to a dynamic queue */
typedef struct ccol_dynamic_queue ccol_dynamic_queue;

/** @brief Opaque handle to a two-way ccol_channel */
typedef struct ccol_channel ccol_channel;

/**
 * @brief Message structure for zero-copy message passing
 *
 * A message hands over the ownership of its data pointer. When a send
 * succeeds the library sets the sender's data pointer to NULL and the
 * receiver becomes the owner, so the receiver must free the memory.
 */
typedef struct c_message_t {
  void *data;  /**< Pointer to the message data (a send transfers ownership) */
  size_t size; /**< Size of the data in bytes */
} c_message_t;

/* ========================================================================== */
/*                          CIRCULAR QUEUE FUNCTIONS                          */
/* ========================================================================== */

/**
 * @brief Create a circular queue with custom memory management
 *
 * Creates a fixed-size queue backed by a circular buffer. A send blocks while
 * the queue is full and waits for free space.
 *
 * @param max_size Maximum number of messages the queue can hold, from 1 to
 * SIZE_MAX / sizeof(c_message_t). A larger value gives ccol_invalid_args: the
 * queue lives in one array of max_size * sizeof(c_message_t) bytes and that
 * multiplication would overflow.
 * @param mmgmt_procs Custom memory management procedures, or NULL to use the
 * default malloc and free
 * @param err_str Optional pointer that gets an error string on failure. Pass
 * NULL to ignore it.
 *
 * @return Pointer to the new circular queue, or NULL on failure
 *
 * @note All operations on this queue are thread-safe
 * @note You must destroy the queue with ccol_circular_queue_destroy() once
 * you are done with it
 *
 * @see ccol_circular_queue_create
 * @see ccol_circular_queue_destroy
 */
ccol_circular_queue *ccol_circular_queue_create_mp(
    size_t max_size, ccol_memmgmt_procs_t *mmgmt_procs, char **err_str);

/**
 * @brief Create a circular queue with default memory management
 *
 * Creates a circular queue that uses the standard malloc and free.
 *
 * @param max_size Maximum number of messages that the queue can hold
 * @param err_str Optional pointer that gets an error string on failure
 *
 * @return Pointer to the new circular queue, or NULL on failure
 */
#define ccol_circular_queue_create(max_size, err_str) \
  ccol_circular_queue_create_mp((max_size), NULL, (err_str))

/**
 * @brief Destroy a circular queue (internal function)
 *
 * @param cq Circular queue to destroy
 *
 * @warning Do not call this function directly. Use the
 * ccol_circular_queue_destroy() macro.
 */
void __ccol_circular_queue_destroy(ccol_circular_queue *cq);

/**
 * @brief Destroy a circular queue and set the pointer to NULL
 *
 * Frees every resource of the queue through __ccol_circular_queue_destroy(),
 * which asserts if the queue still holds messages or if a watcher still
 * watches it. A watcher is a ccol_select() call, a ccol_select_timed() call,
 * or an ccol_event_loop registration (ccol_event_loop_add with
 * ccol_selectable_from_circq or ccol_selectable_from_chan). Drain the queue,
 * call ccol_event_loop_remove(), and only then destroy the queue; letting
 * every ccol_select() and ccol_select_timed() call that watches this queue
 * return works as well.
 *
 * That sequence is safe as written. ccol_event_loop_remove() doesn't wait for
 * a callback it already handed to a reactor thread, so such a callback can be
 * inside this queue when the removal returns, but this destroy blocks until
 * every one of those callbacks is done with the queue and frees the queue
 * only after that.
 *
 * The destroy counts the messages left in the queue only once those
 * callbacks have finished. A write callback that was already running when
 * ccol_event_loop_remove() returned can still send after that, and its
 * message then makes the destroy assert. So when a write registration
 * watches the queue, remove it first, wait for its on_removed callback, and
 * then drain and destroy the queue.
 *
 * A callback that the loop runs for a different registration can run this
 * whole sequence for this queue, whatever num_reactor_threads is. A dispatch
 * the loop collected for this queue but hasn't started yet doesn't hold up
 * the destroy, and it never runs its callback after the removal.
 *
 * @param cq Circular queue to destroy. The macro sets it to NULL afterwards.
 *
 * @warning The macro does not free the messages that stay in the queue
 * @warning Do not destroy a queue that a ccol_select() call, a
 * ccol_select_timed() call, or an ccol_event_loop registration watches. The
 * watcher's waiter node points to this queue's mutex, so that would be a
 * use-after-free, and the function asserts against it just as it asserts
 * against a queue that isn't empty.
 * @warning Do not call this from inside an ccol_event_loop callback that
 * dispatches this same queue, since the destroy would wait for that very
 * callback. It is a caller error and the program stops through
 * _ccol_fatal_err(). Destroy the queue from a thread that doesn't run one of
 * its callbacks.
 * @warning Do not hold an application lock across this call if one of this
 * queue's callbacks takes that lock: the destroy waits for the callback,
 * and the callback would wait for the lock.
 * @note You can call this with a NULL pointer
 * @note The macro evaluates cq exactly once, and cq must be a modifiable
 * lvalue such as a variable or an array element
 */
#define ccol_circular_queue_destroy(cq) \
  _ccol_circular_queue_destroy_impl(    \
      cq, _ccol_uniq(__ccol_cq_destroy_slot, __COUNTER__))

/* Internal body of ccol_circular_queue_destroy. slot is a name from
 * _ccol_uniq(), so the macro can nest inside the argument of another destroy
 * macro and stay -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_circular_queue_destroy_impl(cq, slot) \
  do {                                              \
    __typeof__(cq) *slot = &(cq);                   \
    __ccol_circular_queue_destroy(*slot);           \
    *slot = NULL;                                   \
  } while (0)

/**
 * @brief Send a message to the queue (it blocks, zero-copy)
 *
 * Blocks until the queue has free space and then hands the ownership of the
 * message data to the queue. On success it sets msg->data to NULL.
 *
 * @param cq Circular queue to send to
 * @param msg Message to send. On success the ownership of the data moves to
 * the queue.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or msg is NULL, if msg->data is NULL while
 * msg->size is not 0, or if msg->data is not NULL while msg->size is 0
 * @return ccol_not_permitted if sends on the queue are turned off
 *
 * @note The function waits with no time limit for free space, or for sends
 * on the queue to be turned off.
 * @note Each time it wakes up, the function checks again whether sends are
 * on
 * @note On failure, the caller keeps the ownership of msg->data
 * @note msg->data == NULL with msg->size == 0 is valid (a sentinel message)
 * @note msg->data == NULL with msg->size > 0 is not valid, and neither is
 * msg->data != NULL with msg->size == 0, because a live pointer must carry a
 * payload
 *
 * @see ccol_circq_try_send_zc
 * @see ccol_circq_timed_send_zc
 */
ccol_retval_t ccol_circq_send_zc(ccol_circular_queue *cq, c_message_t *msg);

/**
 * @brief Try to send a message with no block (zero-copy)
 *
 * Tries to send the message and returns ccol_container_full at
 * once if the queue is full. On success the ownership of msg->data moves to
 * the queue.
 *
 * @param cq Circular queue to send to
 * @param msg Message to send. On success the ownership of the data moves to
 * the queue.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or msg is NULL, if msg->data is NULL while
 * msg->size is not 0, or if msg->data is not NULL while msg->size is 0
 * @return ccol_not_permitted if sends on the queue are turned off
 * @return ccol_container_full if the queue is full
 *
 * @note The function never blocks
 * @note On failure, the caller keeps the ownership of msg->data
 *
 * @see ccol_circq_send_zc
 * @see ccol_circq_timed_send_zc
 */
ccol_retval_t ccol_circq_try_send_zc(ccol_circular_queue *cq, c_message_t *msg);

/**
 * @brief Send a message with a timeout (it blocks, zero-copy)
 *
 * Blocks for at most the given timeout while it waits for free space. If
 * space frees up in time it sends the message and hands over the ownership,
 * and if the timeout ends or sends are turned off it returns the matching
 * error code.
 *
 * @param cq Circular queue to send to
 * @param msg Message to send. On success the ownership of the data moves to
 * the queue.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 doesn't wait, and the call then does exactly what
 * ccol_circq_try_send_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline the
 * clock can hold, which in practice is a wait with no end, and never wraps
 * into the past.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or msg is NULL, if msg->data is NULL while
 * msg->size is not 0, or if msg->data is not NULL while msg->size is 0
 * @return ccol_not_permitted if sends on the queue are turned off
 * @return ccol_container_full if timeout_us is 0 and the queue is full
 * @return ccol_timed_out if the timeout ends before free space arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for at most the duration of the timeout
 * @note Each time it wakes up, the function checks again whether sends are
 * on
 * @note On failure, the caller keeps the ownership of msg->data
 * @note The timeout is measured against CLOCK_MONOTONIC, so changing the
 * system's wall clock (an administrator, NTP, or a virtual machine that
 * resumes) doesn't make it longer or shorter; the call waits for the
 * duration the caller gave.
 *
 * @see ccol_circq_send_zc
 * @see ccol_circq_try_send_zc
 */
ccol_retval_t ccol_circq_timed_send_zc(ccol_circular_queue *cq,
                                       c_message_t *msg, uint64_t timeout_us);

/**
 * @brief Receive a message from the queue (it blocks, zero-copy)
 *
 * Blocks until a message arrives and then hands the ownership of its data to
 * the caller in target_buf.
 *
 * @param cq Circular queue to receive from
 * @param target_buf Buffer that receives the message, and with it the
 * ownership of the data.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or target_buf is NULL
 *
 * @note The function blocks with no time limit while it waits for a message.
 * @note The function stays blocked while sends on the queue are off
 * @note The caller gets the ownership of target_buf->data and must free it
 *
 * @see ccol_circq_try_recv_zc
 * @see ccol_circq_timed_recv_zc
 */
ccol_retval_t ccol_circq_recv_zc(ccol_circular_queue *cq,
                                 c_message_t *target_buf);

/**
 * @brief Try to receive a message with no block (zero-copy)
 *
 * Tries to receive a message and returns ccol_container_empty at
 * once if the queue is empty. On success the ownership of the message data
 * moves to the caller.
 *
 * @param cq Circular queue to receive from
 * @param target_buf Buffer that receives the message, and with it the
 * ownership of the data.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if cq or target_buf is NULL
 * @return ccol_container_empty if the queue is empty
 *
 * @note The function never blocks
 * @note On success, the caller gets the ownership of target_buf->data
 *
 * @see ccol_circq_recv_zc
 * @see ccol_circq_timed_recv_zc
 */
ccol_retval_t ccol_circq_try_recv_zc(ccol_circular_queue *cq,
                                     c_message_t *target_buf);

/**
 * @brief Receive a message with a timeout (it blocks, zero-copy)
 *
 * Blocks for at most the given timeout while it waits for a message. If one
 * arrives in time it is received and its ownership moves to the caller, and
 * if the timeout ends the function returns ccol_timed_out.
 *
 * @param cq Circular queue to receive from
 * @param target_buf Buffer that receives the message, and with it the
 * ownership of the data.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 doesn't wait, and the call then does exactly what
 * ccol_circq_try_recv_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline the
 * clock can hold, which in practice is a wait with no end, and never wraps
 * into the past.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if cq or target_buf is NULL
 * @return ccol_container_empty if timeout_us is 0 and the queue is empty
 * @return ccol_timed_out if the timeout ends before a message arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for at most the duration of the timeout
 * @note On success, the caller gets the ownership of target_buf->data
 * @note The timeout is measured against CLOCK_MONOTONIC, so changing the
 * system's wall clock (an administrator, NTP, or a virtual machine that
 * resumes) doesn't make it longer or shorter; the call waits for the
 * duration the caller gave.
 *
 * @see ccol_circq_recv_zc
 * @see ccol_circq_try_recv_zc
 */
ccol_retval_t ccol_circq_timed_recv_zc(ccol_circular_queue *cq,
                                       c_message_t *target_buf,
                                       uint64_t timeout_us);

/**
 * @brief Turn off sends on the queue
 *
 * Stops every new send and wakes each thread blocked inside a send, so those
 * senders return ccol_not_permitted. Receives are not affected.
 *
 * A queue whose sends are off is ready for the write direction of
 * ccol_select() and of a ccol_event_loop registration, the same way a pipe
 * whose reader has gone is ready for POLLOUT. The function wakes every write
 * waiter, and the try_send the waiter then makes returns ccol_not_permitted
 * at once.
 *
 * @param cq Circular queue that gets sends turned off
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq is NULL
 *
 * @note The function wakes every blocked sender at once with a broadcast
 * @note ccol_circq_enable_sending() turns sends on again
 * @note Receives are not affected
 *
 * @see ccol_circq_enable_sending
 */
ccol_retval_t ccol_circq_disable_sending(ccol_circular_queue *cq);

/**
 * @brief Turn on sends on the queue
 *
 * Turns sends back on after a call turned them off, and wakes every thread
 * blocked waiting to send so those senders can go on.
 *
 * @param cq Circular queue that gets sends turned on
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq is NULL
 *
 * @note The function wakes every blocked sender with a broadcast
 * @note A thread can try to send at once if the queue has free space
 * @note From this call on, a full queue is not ready for the write direction
 * of ccol_select() or of a ccol_event_loop registration until it has room
 *
 * @see ccol_circq_disable_sending
 */
ccol_retval_t ccol_circq_enable_sending(ccol_circular_queue *cq);

/**
 * @brief Get the current message count of the queue
 *
 * Gives the number of messages the queue holds at the moment.
 *
 * @param cq Circular queue to ask
 *
 * @return Number of messages in the queue, or (size_t)-1 on an error
 *
 * @note The count is a thread-safe snapshot
 * @note A return value of (size_t)-1 means an error (a NULL queue)
 */
size_t ccol_circq_msg_count(ccol_circular_queue *cq);

/* ========================================================================== */
/*                          DYNAMIC QUEUE FUNCTIONS                           */
/* ========================================================================== */

/**
 * @brief Create a dynamic queue with custom memory management
 *
 * Creates a queue with no size limit on top of a doubly-linked list. A send
 * never blocks, but it fails if memory can't be allocated, and the queue can
 * grow up to ccol_max_elem_count messages.
 *
 * @param mmgmt_procs Custom memory management procedures, or NULL to use the
 * default malloc and free
 * @param err_str Optional pointer that gets an error string on failure. Pass
 * NULL to ignore it.
 *
 * @return Pointer to the new dynamic queue, or NULL on failure
 *
 * @note A send never blocks; only the free memory limits it.
 * @note The maximum size is ccol_max_elem_count, defined in common.h
 * @note You must destroy the queue with ccol_dynamic_queue_destroy() once you
 * are done with it
 *
 * @see ccol_dynamic_queue_create
 * @see ccol_dynamic_queue_destroy
 */
ccol_dynamic_queue *ccol_dynamic_queue_create_mp(
    ccol_memmgmt_procs_t *mmgmt_procs, char **err_str);

/**
 * @brief Create a dynamic queue with default memory management
 *
 * Creates a dynamic queue that uses the standard malloc and free.
 *
 * @param err_str Optional pointer that gets an error string on failure
 *
 * @return Pointer to the new dynamic queue, or NULL on failure
 */
#define ccol_dynamic_queue_create(err_str) \
  ccol_dynamic_queue_create_mp(NULL, (err_str))

/**
 * @brief Destroy a dynamic queue (internal function)
 *
 * @param dq Dynamic queue to destroy
 *
 * @warning Do not call this function directly. Use the
 * ccol_dynamic_queue_destroy() macro.
 */
void __ccol_dynamic_queue_destroy(ccol_dynamic_queue *dq);

/**
 * @brief Destroy a dynamic queue and set the pointer to NULL
 *
 * Frees every resource of the queue, every node of the linked list included,
 * through __ccol_dynamic_queue_destroy(), which asserts if the queue still
 * holds messages or if a watcher still watches it. A watcher is a
 * ccol_select() call, a ccol_select_timed() call, or an ccol_event_loop
 * registration (ccol_event_loop_add with ccol_selectable_from_dynq). Drain
 * the queue, call ccol_event_loop_remove(), and only then destroy the queue;
 * letting every ccol_select() and ccol_select_timed() call that watches this
 * queue return works as well.
 *
 * That sequence is safe as written. ccol_event_loop_remove() doesn't wait for
 * a callback it already handed to a reactor thread, so such a callback can be
 * inside this queue when the removal returns, but this destroy blocks until
 * every one of those callbacks is done with the queue and frees the queue
 * only after that.
 *
 * The destroy counts the messages left in the queue only once those
 * callbacks have finished. A write callback that was already running when
 * ccol_event_loop_remove() returned can still send after that, and its
 * message then makes the destroy assert. So when a write registration
 * watches the queue, remove it first, wait for its on_removed callback, and
 * then drain and destroy the queue.
 *
 * A callback that the loop runs for a different registration can run this
 * whole sequence for this queue, whatever num_reactor_threads is. A dispatch
 * the loop collected for this queue but hasn't started yet doesn't hold up
 * the destroy, and it never runs its callback after the removal.
 *
 * @param dq Dynamic queue to destroy. The macro sets it to NULL afterwards.
 *
 * @warning The macro does not free the messages that stay in the queue
 * @warning Do not destroy a queue that a ccol_select() call, a
 * ccol_select_timed() call, or an ccol_event_loop registration watches. The
 * watcher's waiter node points to this queue's mutex, so that would be a
 * use-after-free, and the function asserts against it just as it asserts
 * against a queue that isn't empty.
 * @warning Do not call this from inside an ccol_event_loop callback that
 * dispatches this same queue, since the destroy would wait for that very
 * callback. It is a caller error and the program stops through
 * _ccol_fatal_err(). Destroy the queue from a thread that doesn't run one of
 * its callbacks.
 * @warning Do not hold an application lock across this call if one of this
 * queue's callbacks takes that lock: the destroy waits for the callback,
 * and the callback would wait for the lock.
 * @note You can call this with a NULL pointer
 * @note The macro evaluates dq exactly once, and dq must be a modifiable
 * lvalue such as a variable or an array element
 */
#define ccol_dynamic_queue_destroy(dq) \
  _ccol_dynamic_queue_destroy_impl(    \
      dq, _ccol_uniq(__ccol_dq_destroy_slot, __COUNTER__))

/* Internal body of ccol_dynamic_queue_destroy. slot is a name from
 * _ccol_uniq(), so the macro can nest inside the argument of another destroy
 * macro and stay -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_dynamic_queue_destroy_impl(dq, slot) \
  do {                                             \
    __typeof__(dq) *slot = &(dq);                  \
    __ccol_dynamic_queue_destroy(*slot);           \
    *slot = NULL;                                  \
  } while (0)

/**
 * @brief Send a message to the dynamic queue (no block, zero-copy)
 *
 * Sends a message without blocking. It allocates a new list node, puts the
 * message at the tail of the queue and hands the ownership of msg->data to
 * the queue.
 *
 * @param dq Dynamic queue to send to
 * @param msg Message to send. On success the ownership of the data moves to
 * the queue.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if dq or msg is NULL, if msg->data is NULL while
 * msg->size is not 0, or if msg->data is not NULL while msg->size is 0
 * @return ccol_not_permitted if sends on the queue are turned off
 * @return ccol_container_full if the queue holds ccol_max_elem_count messages
 * @return ccol_not_enough_memory if the allocation of the node fails
 *
 * @note The function never blocks; it returns at once with a success code
 * or an error code.
 * @note On failure, the caller keeps the ownership of msg->data
 * @note There is no blocking send and no send with a timeout, since the
 * queue has no size limit by design.
 * @note Only ccol_max_elem_count and the free memory limit the queue
 * @note msg->data == NULL with msg->size == 0 is valid (a sentinel message)
 * @note msg->data == NULL with msg->size > 0 is not valid, and neither is
 * msg->data != NULL with msg->size == 0, because a live pointer must carry a
 * payload
 *
 * @see ccol_dynmq_recv_zc
 */
ccol_retval_t ccol_dynmq_send_zc(ccol_dynamic_queue *dq, c_message_t *msg);

/**
 * @brief Receive a message from the dynamic queue (it blocks, zero-copy)
 *
 * Blocks until a message arrives, then takes it off the head of the queue
 * and hands its ownership to the caller.
 *
 * @param dq Dynamic queue to receive from
 * @param target_buf Buffer that receives the message, and with it the
 * ownership of the data.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if dq or target_buf is NULL
 *
 * @note The function blocks with no time limit while it waits for a message.
 * @note The function stays blocked while sends on the queue are off
 * @note The caller gets the ownership of target_buf->data and must free it
 *
 * @see ccol_dynmq_try_recv_zc
 * @see ccol_dynmq_timed_recv_zc
 */
ccol_retval_t ccol_dynmq_recv_zc(ccol_dynamic_queue *dq,
                                 c_message_t *target_buf);

/**
 * @brief Try to receive a message with no block (zero-copy)
 *
 * Tries to receive a message and returns ccol_container_empty at
 * once if the queue is empty.
 *
 * @param dq Dynamic queue to receive from
 * @param target_buf Buffer that receives the message, and with it the
 * ownership of the data.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if dq or target_buf is NULL
 * @return ccol_container_empty if the queue is empty
 *
 * @note The function never blocks
 * @note On success, the caller gets the ownership of target_buf->data
 *
 * @see ccol_dynmq_recv_zc
 * @see ccol_dynmq_timed_recv_zc
 */
ccol_retval_t ccol_dynmq_try_recv_zc(ccol_dynamic_queue *dq,
                                     c_message_t *target_buf);

/**
 * @brief Receive a message with a timeout (it blocks, zero-copy)
 *
 * Blocks for at most the given timeout while it waits for a message. If one
 * arrives in time it is received and its ownership moves to the caller.
 *
 * @param dq Dynamic queue to receive from
 * @param target_buf Buffer that receives the message, and with it the
 * ownership of the data.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 doesn't wait, and the call then does exactly what
 * ccol_dynmq_try_recv_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline the
 * clock can hold, which in practice is a wait with no end, and never wraps
 * into the past.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if dq or target_buf is NULL
 * @return ccol_container_empty if timeout_us is 0 and the queue is empty
 * @return ccol_timed_out if the timeout ends before a message arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for at most the duration of the timeout
 * @note On success, the caller gets the ownership of target_buf->data
 * @note The timeout is measured against CLOCK_MONOTONIC, so changing the
 * system's wall clock (an administrator, NTP, or a virtual machine that
 * resumes) doesn't make it longer or shorter; the call waits for the
 * duration the caller gave.
 *
 * @see ccol_dynmq_recv_zc
 * @see ccol_dynmq_try_recv_zc
 */
ccol_retval_t ccol_dynmq_timed_recv_zc(ccol_dynamic_queue *dq,
                                       c_message_t *target_buf,
                                       uint64_t timeout_us);

/**
 * @brief Turn off sends on the dynamic queue
 *
 * Stops every new send. A dynamic queue never has blocked senders the way a
 * circular queue can, because its send never blocks, so this call only
 * affects the sends that come later.
 *
 * A queue whose sends are off is ready for the write direction of
 * ccol_select() and of a ccol_event_loop registration, the same way a pipe
 * whose reader has gone is ready for POLLOUT. The function wakes every write
 * waiter, and the send the waiter then makes returns ccol_not_permitted at
 * once.
 *
 * @param dq Dynamic queue that gets sends turned off
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if dq is NULL
 *
 * @note ccol_dynmq_enable_sending() turns sends on again
 * @note Receives are not affected
 *
 * @see ccol_dynmq_enable_sending
 */
ccol_retval_t ccol_dynmq_disable_sending(ccol_dynamic_queue *dq);

/**
 * @brief Turn on sends on the dynamic queue
 *
 * Turns sends back on after a call turned them off.
 *
 * @param dq Dynamic queue that gets sends turned on
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if dq is NULL
 *
 * @see ccol_dynmq_disable_sending
 */
ccol_retval_t ccol_dynmq_enable_sending(ccol_dynamic_queue *dq);

/**
 * @brief Get the current message count of the dynamic queue
 *
 * Gives the number of messages the queue holds at the moment.
 *
 * @param dq Dynamic queue to ask
 *
 * @return Number of messages in the queue, or (size_t)-1 on an error
 *
 * @note The count is a thread-safe snapshot
 * @note A return value of (size_t)-1 means an error (a NULL queue)
 */
size_t ccol_dynmq_msg_count(ccol_dynamic_queue *dq);

/* ========================================================================== */
/*                            CHANNEL FUNCTIONS                               */
/* ========================================================================== */

/**
 * @brief Create a two-way ccol_channel with custom memory management
 *
 * Creates a ccol_channel, a pair of circular queues that carries two-way
 * communication between an owner thread and the worker threads. The thread
 * that calls this function becomes the owner, and on every call the channel
 * picks the direction itself from the calling thread's ID.
 *
 * @param max_size Maximum number of messages each direction can hold. Each
 * direction is one circular queue, so the bound is the same as for the
 * max_size of ccol_circular_queue_create_mp: SIZE_MAX / sizeof(c_message_t).
 * @param mmgmt_procs Custom memory management procedures, or NULL to use the
 * default malloc and free
 * @param err_str Optional pointer that gets an error string on failure. Pass
 * NULL to ignore it.
 *
 * @return Pointer to the new ccol_channel, or NULL on failure
 *
 * @note The channel holds two circular queues: ccol_owner_to_workers and
 * ccol_workers_to_owner
 * @note The thread that creates the channel is the owner, and every other
 * thread is a worker.
 * @note The channel picks the direction itself from the ID of the thread
 * @note You must destroy the ccol_channel with ccol_channel_destroy() once
 * you are done with it
 * @note The owner thread must stay alive for as long as the ccol_channel is
 * in use. The channel routes a message by comparing the calling thread's ID
 * with the ID it took at creation, and once the first owner exits the OS can
 * give the same thread-ID value to a new, unrelated thread, which the channel
 * then silently routes as the owner too.
 *
 * @see ccol_channel_create
 * @see ccol_channel_destroy
 */
ccol_channel *ccol_channel_create_mp(size_t max_size,
                                     ccol_memmgmt_procs_t *mmgmt_procs,
                                     char **err_str);

/**
 * @brief Create a two-way ccol_channel with default memory management
 *
 * Creates a ccol_channel that uses the standard malloc and free.
 *
 * @param max_size Maximum number of messages that each direction can hold
 * @param err_str Optional pointer that gets an error string on failure
 *
 * @return Pointer to the new ccol_channel, or NULL on failure
 */
#define ccol_channel_create(max_size, err_str) \
  ccol_channel_create_mp((max_size), NULL, (err_str))

/**
 * @brief Destroy a ccol_channel (internal function)
 *
 * @param ch Channel to destroy
 *
 * @warning Do not call this function directly. Use the ccol_channel_destroy()
 * macro.
 */
void __ccol_channel_destroy(ccol_channel *ch);

/**
 * @brief Destroy a ccol_channel and set the pointer to NULL
 *
 * Frees every resource of the ccol_channel, both of its circular queues
 * included, through __ccol_channel_destroy(), which asserts if a queue below
 * the channel still holds messages or if a watcher still watches a
 * direction. A watcher is a ccol_select() call, a ccol_select_timed() call,
 * or an ccol_event_loop registration (ccol_event_loop_add with
 * ccol_selectable_from_chan). Drain both directions, call
 * ccol_event_loop_remove(), and only then destroy the channel; letting every
 * ccol_select() and ccol_select_timed() call that watches one of the
 * directions return works as well.
 *
 * That sequence is safe as written. ccol_event_loop_remove() doesn't wait for
 * a callback it already handed to a reactor thread, so such a callback can be
 * inside one of the channel's queues when the removal returns, but this
 * destroy blocks until every one of those callbacks is done with that queue
 * and frees the queue only after that.
 *
 * A callback that the loop runs for a different registration can run this
 * whole sequence for this channel, whatever num_reactor_threads is. A
 * dispatch the loop collected for the channel but hasn't started yet doesn't
 * hold up the destroy, and it never runs its callback after the removal.
 *
 * @param ch Channel to destroy. The macro sets it to NULL afterwards.
 *
 * @warning The macro does not free messages left in either direction
 * @warning Do not destroy a ccol_channel that a ccol_select() call, a
 * ccol_select_timed() call, or an ccol_event_loop registration watches. The
 * watcher's waiter node points to the mutex of the queue below the channel,
 * so that would be a use-after-free, and the function asserts against it
 * just as it asserts against messages left in one of the directions.
 * @warning Do not call this from inside an ccol_event_loop callback that
 * dispatches one of this ccol_channel's directions, since the destroy would
 * wait for that very callback. It is a caller error and the program stops
 * through _ccol_fatal_err().
 * @warning Do not hold an application lock across this call if one of this
 * ccol_channel's callbacks takes that lock: the destroy waits for the
 * callback, and the callback would wait for the lock.
 * @note You can call this with a NULL pointer
 * @note The macro evaluates ch exactly once, and ch must be a modifiable
 * lvalue such as a variable or an array element
 */
#define ccol_channel_destroy(ch) \
  _ccol_channel_destroy_impl(    \
      ch, _ccol_uniq(__ccol_channel_destroy_slot, __COUNTER__))

/* Internal body of ccol_channel_destroy. slot is a name from _ccol_uniq(),
 * so the macro can nest inside the argument of another destroy macro and
 * stay -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_channel_destroy_impl(ch, slot) \
  do {                                       \
    __typeof__(ch) *slot = &(ch);            \
    __ccol_channel_destroy(*slot);           \
    *slot = NULL;                            \
  } while (0)

/**
 * @brief Send a message through the ccol_channel (it blocks, zero-copy)
 *
 * The function picks the queue itself, from the thread that calls it:
 * - the owner thread sends to the ccol_owner_to_workers queue
 * - a worker thread sends to the ccol_workers_to_owner queue
 *
 * @param ch Channel to send through
 * @param msg Message to send. On success the ownership of the data moves to
 * the channel.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch or msg is NULL, if msg->data is NULL while
 * msg->size is not 0, or if msg->data is not NULL while msg->size is 0
 * @return ccol_not_permitted if sends on the chosen direction are turned off
 *
 * @note The function blocks until the chosen queue has free space
 * @note The function picks the direction itself from the ID of the thread
 * @note On failure, the caller keeps the ownership of msg->data
 *
 * @see ccol_chan_try_send_zc
 * @see ccol_chan_timed_send_zc
 * @see ccol_chan_recv_zc
 */
ccol_retval_t ccol_chan_send_zc(ccol_channel *ch, c_message_t *msg);

/**
 * @brief Try to send a message with no block (zero-copy)
 *
 * Tries to send the message through the queue the channel picks
 * itself, and returns at once if that queue is full.
 *
 * @param ch Channel to send through
 * @param msg Message to send. On success the ownership of the data moves to
 * the channel.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch or msg is NULL, if msg->data is NULL while
 * msg->size is not 0, or if msg->data is not NULL while msg->size is 0
 * @return ccol_not_permitted if sends on the chosen direction are turned off
 * @return ccol_container_full if the chosen queue is full
 *
 * @note The function never blocks
 * @note The function picks the direction itself from the ID of the thread
 *
 * @see ccol_chan_send_zc
 * @see ccol_chan_timed_send_zc
 */
ccol_retval_t ccol_chan_try_send_zc(ccol_channel *ch, c_message_t *msg);

/**
 * @brief Send a message with a timeout (it blocks, zero-copy)
 *
 * Blocks for at most the given timeout while it waits for free space in the
 * queue the channel picks itself.
 *
 * @param ch Channel to send through
 * @param msg Message to send. On success the ownership of the data moves to
 * the channel.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 doesn't wait, and the call then does exactly what
 * ccol_chan_try_send_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline the
 * clock can hold, which in practice is a wait with no end, and never wraps
 * into the past.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch or msg is NULL, if msg->data is NULL while
 * msg->size is not 0, or if msg->data is not NULL while msg->size is 0
 * @return ccol_not_permitted if sends on the chosen direction are turned off
 * @return ccol_container_full if timeout_us is 0 and the chosen queue is full
 * @return ccol_timed_out if the timeout ends before free space arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for at most the duration of the timeout
 * @note The function picks the direction itself from the ID of the thread
 * @note The timeout is measured against CLOCK_MONOTONIC, so changing the
 * system's wall clock (an administrator, NTP, or a virtual machine that
 * resumes) doesn't make it longer or shorter; the call waits for the
 * duration the caller gave.
 *
 * @see ccol_chan_send_zc
 * @see ccol_chan_try_send_zc
 */
ccol_retval_t ccol_chan_timed_send_zc(ccol_channel *ch, c_message_t *msg,
                                      uint64_t timeout_us);

/**
 * @brief Receive a message from the ccol_channel (it blocks, zero-copy)
 *
 * The function picks the queue itself, from the thread that calls it:
 * - the owner thread receives from the ccol_workers_to_owner queue
 * - a worker thread receives from the ccol_owner_to_workers queue
 *
 * @param ch Channel to receive from
 * @param target_buf Buffer that receives the message, and with it the
 * ownership of the data.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if ch or target_buf is NULL
 *
 * @note The function blocks until a message arrives in the chosen queue
 * @note The function picks the direction itself from the ID of the thread
 * @note The caller gets the ownership of target_buf->data and must free it
 *
 * @see ccol_chan_try_recv_zc
 * @see ccol_chan_timed_recv_zc
 * @see ccol_chan_send_zc
 */
ccol_retval_t ccol_chan_recv_zc(ccol_channel *ch, c_message_t *target_buf);

/**
 * @brief Try to receive a message with no block (zero-copy)
 *
 * Tries to receive a message right away from the queue the channel picks
 * itself, and returns at once if that queue is empty.
 *
 * @param ch Channel to receive from
 * @param target_buf Buffer that receives the message, and with it the
 * ownership of the data.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if ch or target_buf is NULL
 * @return ccol_container_empty if the chosen queue is empty
 *
 * @note The function never blocks
 * @note The function picks the direction itself from the ID of the thread
 *
 * @see ccol_chan_recv_zc
 * @see ccol_chan_timed_recv_zc
 */
ccol_retval_t ccol_chan_try_recv_zc(ccol_channel *ch, c_message_t *target_buf);

/**
 * @brief Receive a message with a timeout (it blocks, zero-copy)
 *
 * Blocks for at most the given timeout while it waits for a message in the
 * queue the channel picks itself.
 *
 * @param ch Channel to receive from
 * @param target_buf Buffer that receives the message, and with it the
 * ownership of the data.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 doesn't wait, and the call then does exactly what
 * ccol_chan_try_recv_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline the
 * clock can hold, which in practice is a wait with no end, and never wraps
 * into the past.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if ch or target_buf is NULL
 * @return ccol_container_empty if timeout_us is 0 and the chosen queue is empty
 * @return ccol_timed_out if the timeout ends before a message arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for at most the duration of the timeout
 * @note The function picks the direction itself from the ID of the thread
 * @note The timeout is measured against CLOCK_MONOTONIC, so changing the
 * system's wall clock (an administrator, NTP, or a virtual machine that
 * resumes) doesn't make it longer or shorter; the call waits for the
 * duration the caller gave.
 *
 * @see ccol_chan_recv_zc
 * @see ccol_chan_try_recv_zc
 */
ccol_retval_t ccol_chan_timed_recv_zc(ccol_channel *ch, c_message_t *target_buf,
                                      uint64_t timeout_us);

/**
 * @brief Direction for the control operations of a ccol_channel
 */
typedef enum ccol_channel_direction {
  ccol_owner_to_workers = 0, /**< Messages from the owner to the workers */
  ccol_workers_to_owner      /**< Messages from the workers to the owner */
} ccol_channel_direction;

/**
 * @brief Turn off sends on one direction of a ccol_channel
 *
 * Stops every new send on the given direction and wakes each thread blocked
 * inside a send on it. That direction's queue is then ready for a write
 * selectable of ccol_select() and for a write registration of a
 * ccol_event_loop, and the send the waiter makes returns ccol_not_permitted
 * at once, as with ccol_circq_disable_sending().
 *
 * @param ch Channel to change
 * @param d Direction that gets sends turned off (ccol_owner_to_workers or
 * ccol_workers_to_owner)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch is NULL or d is not valid
 *
 * @note The function wakes every blocked sender on the given direction
 * @note ccol_chan_enable_sending() turns sends on again
 * @note The other direction is not affected
 *
 * @see ccol_chan_enable_sending
 */
ccol_retval_t ccol_chan_disable_sending(ccol_channel *ch,
                                        ccol_channel_direction d);

/**
 * @brief Turn on sends on one direction of a ccol_channel
 *
 * Turns sends on the given direction back on after a call turned them off,
 * and wakes every thread blocked waiting to send.
 *
 * @param ch Channel to change
 * @param d Direction that gets sends turned on (ccol_owner_to_workers or
 * ccol_workers_to_owner)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch is NULL or d is not valid
 *
 * @note The function wakes every blocked sender with a broadcast
 * @note A thread can try to send right away if the queue has free space
 * @note The other direction is not affected
 *
 * @see ccol_chan_disable_sending
 */
ccol_retval_t ccol_chan_enable_sending(ccol_channel *ch,
                                       ccol_channel_direction d);

/**
 * @brief Get the current message count of one ccol_channel direction
 *
 * Gives the number of messages the given direction's queue holds at the
 * moment.
 *
 * @param ch Channel to ask
 * @param d Direction to ask about (ccol_owner_to_workers or
 * ccol_workers_to_owner)
 *
 * @return Number of messages in the given direction, or (size_t)-1 on an error
 *
 * @note The count is a thread-safe snapshot
 * @note A return value of (size_t)-1 means an error: the ccol_channel is
 * NULL, or the direction is not valid.
 */
size_t ccol_chan_msg_count(ccol_channel *ch, ccol_channel_direction d);

/* ========================================================================== */
/*                             CCOL_SELECT API */
/* ========================================================================== */

/**
 * @brief Type tag of a selectable for ccol_select
 */
typedef enum {
  ccol_selectable_circq, /**< Holds a ccol_circular_queue pointer */
  ccol_selectable_dynq,  /**< Holds a ccol_dynamic_queue pointer  */
  ccol_selectable_fd,    /**< Holds a raw file descriptor     */
} ccol_selectable_type;

/**
 * @brief The direction that a ccol_selectable waits on
 *
 * ccol_select_read waits until the queue holds at least one message to
 * receive. ccol_select_write waits until a send on the queue doesn't have to
 * wait, which means the queue has room for at least one send or its sends
 * are turned off. For a ccol_circular_queue the test is msg_count < max_size
 * || writing_disabled. For a ccol_dynamic_queue it is writing_disabled ||
 * msg_count < ccol_max_elem_count, where ccol_max_elem_count is the same
 * ccol_container_full ceiling that ccol_dynmq_send_zc() applies. A queue
 * whose sends are off is ready the same way a pipe whose reader has gone is
 * ready for POLLOUT, and the caller's send then returns ccol_not_permitted at
 * once.
 *
 * ccol_select() neither consumes nor reserves either direction, so once it
 * wakes the caller makes the call itself: ccol_circq_try_recv_zc or
 * ccol_dynmq_try_recv_zc after a win on the read direction, and
 * ccol_circq_try_send_zc or ccol_dynmq_send_zc after a win on the write
 * direction. As with POSIX select(2) a TOCTOU race is possible, and that's
 * why that call must not block.
 */
typedef enum {
  ccol_select_read,  /**< Wait for one readable message or more      */
  ccol_select_write, /**< Wait for space to send one message or more */
} ccol_select_dir;

/**
 * @brief Tagged union for one queue that ccol_select watches
 *
 * Build one with ccol_selectable_from_circq(q, dir),
 * ccol_selectable_from_dynq(q, dir), or ccol_selectable_from_chan(ch, dir).
 * Pass ccol_select_read to wait for a message to arrive, or ccol_select_write
 * to wait until the queue has room. For a channel, the right queue below it
 * is found when the selectable is built, from the calling thread's ID, the
 * same way ccol_chan_recv_zc() and ccol_chan_send_zc() find it.
 *
 * Whatever the type of selectable, ccol_select() never does the receive or
 * the send itself and only reports readiness. See the documentation of
 * ccol_select().
 */
typedef struct {
  ccol_selectable_type type;
  ccol_select_dir dir;
  union {
    ccol_circular_queue *cq;
    ccol_dynamic_queue *dq;
    int fd; /**< In use when type == ccol_selectable_fd. It must be >= 0. */
  };
} ccol_selectable;

/** @brief Build a selectable from a ccol_circular_queue pointer
 *
 *  @param q_       ccol_circular_queue pointer to watch
 *  @param sel_dir  ccol_select_read or ccol_select_write
 */
#define ccol_selectable_from_circq(q_, sel_dir) \
  ((ccol_selectable){                           \
      .type = ccol_selectable_circq, .dir = (sel_dir), .cq = (q_)})

/** @brief Build a selectable from a ccol_dynamic_queue pointer
 *
 *  @param q_       ccol_dynamic_queue pointer to watch
 *  @param sel_dir  ccol_select_read or ccol_select_write
 */
#define ccol_selectable_from_dynq(q_, sel_dir) \
  ((ccol_selectable){                          \
      .type = ccol_selectable_dynq, .dir = (sel_dir), .dq = (q_)})

/** @brief Build a selectable from a raw file descriptor
 *
 *  When the array holds at least one fd selectable, ccol_select uses
 *  epoll(7) internally and bridges the queue selectables of the same array
 *  with one eventfd(2) per waiter, so a single epoll_wait call reports the
 *  readiness of fds and queues alike.
 *
 *  ccol_select() never reads from the fd or writes to it, it only reports
 *  readiness (see the documentation of ccol_select()), and the caller then
 *  does its own read(2), recv(2), write(2), or send(2) on fd_.
 *
 *  @param fd_      File descriptor to watch (it must be >= 0)
 *  @param sel_dir  ccol_select_read  -> EPOLLIN (readable)
 *                  ccol_select_write -> EPOLLOUT (writable)
 *
 *  @note A read selectable always also has EPOLLRDHUP, EPOLLERR, and EPOLLHUP.
 *        A write selectable always also has EPOLLERR and EPOLLHUP.
 */
#define ccol_selectable_from_fd(fd_, sel_dir) \
  ((ccol_selectable){.type = ccol_selectable_fd, .dir = (sel_dir), .fd = (fd_)})

/**
 * @brief Give a ccol_selectable for a ccol_channel, in the direction that
 *        the caller asks for, with the queue resolved for the thread that
 *        calls it.
 *
 * The direction picks the queue, and with it the waiter list that
 * ccol_select() uses internally:
 *
 *   ccol_select_read: the owner reads from workers_to_owner_cq, and a worker
 *                     reads from owner_to_workers_cq. This is the same as
 *                     ccol_chan_recv_zc.
 *   ccol_select_write: the owner writes to owner_to_workers_cq, and a worker
 *                     writes to workers_to_owner_cq. This is the same as
 *                     ccol_chan_send_zc.
 *
 * The queue is resolved once, from the identity of the calling thread, and
 * from then on the ccol_selectable watches that one queue, whichever thread
 * later waits on it or runs a callback for it.
 *
 * Readiness of the resolved queue works as for any other queue selectable:
 * the caller has to do the receive or the send itself, because ccol_select()
 * and ccol_event_loop only report that the queue is ready.
 *
 * Once ccol_select() returns on the thread that built the selectable,
 * ccol_chan_try_recv_zc() and ccol_chan_try_send_zc() resolve to the same
 * queue, and so do ccol_circq_try_recv_zc() and ccol_circq_try_send_zc() on
 * sel.cq.
 *
 * Inside an ccol_event_loop callback, use ccol_circq_try_recv_zc() and
 * ccol_circq_try_send_zc() on the cq of the ccol_selectable the callback
 * receives, never a ccol_chan_* call. The callback runs on one of the loop's
 * threads, which the channel counts as a worker whoever built the
 * selectable, so a ccol_chan_* call there resolves for a worker, and for a
 * selectable the owner built that's the other queue. For example, a
 * selectable the owner built for ccol_select_read watches
 * workers_to_owner_cq, but a ccol_chan_try_recv_zc() in its on_readable
 * callback receives from owner_to_workers_cq instead:
 * it takes a message the owner sent to its workers and leaves the message
 * that made the callback run in the queue.
 *
 * @param ch  Channel to resolve. A NULL value gives a selectable that
 * ccol_select() refuses.
 * @param dir ccol_select_read or ccol_select_write
 * @return ccol_selectable that holds the ccol_circular_queue for that
 * direction
 */
ccol_selectable ccol_selectable_from_chan(ccol_channel *ch,
                                          ccol_select_dir dir);

/**
 * @brief Wait until one of n selectables becomes readable or writable
 *
 * Blocks until at least one selectable is ready for its own direction, then
 * sets *ready_index and returns. ccol_select() never does the receive or the
 * send itself, for a queue selectable or for an fd selectable, so right after
 * the call returns the caller must do its own receive or send on the
 * selectable that won: ccol_circq_try_recv_zc(), ccol_dynmq_try_recv_zc(),
 * ccol_circq_try_send_zc(), or ccol_dynmq_send_zc() for a queue selectable,
 * and read(2), recv(2), write(2), or send(2) on selectables[*ready_index].fd
 * for an fd selectable. This contract is TOCTOU-safe and the same for both
 * directions. Another consumer or producer can win the race between the
 * return of ccol_select() and the caller's own call, so that call must not
 * block and the caller must check its result.
 *
 * One array can mix read selectables and write selectables, since each
 * selectable in it can have any direction.
 *
 * When several selectables are ready they share the calls fairly. Each call
 * checks the selectables in rotation order, starting from an index that moves
 * on with every call the thread makes, and a ready fd selectable takes part
 * in that check the same way a ready queue does. So every selectable that
 * stays ready wins its share of the calls, and a ready selectable is only a
 * few calls behind any other: an fd that a worker watches for a stop request
 * gets reported even though a queue next to it never runs empty. Which of
 * several ready selectables a given call reports is not specified, so a
 * caller that needs a priority between them must check the more important
 * one itself.
 *
 * The function allocates one waiter node on the heap per selectable. Any
 * number of threads can call ccol_select on the same queue at the same time,
 * with no fixed limit on the number of waiters. For each message a producer
 * sends, and for each slot it frees, the library wakes exactly one waiter,
 * never all of them at once. A woken caller can leave without acting on that
 * queue, when its deadline ends, when another selectable of the same array
 * wins, or when the call fails, and such a caller hands the wake on to the
 * next waiter if the queue is still ready for that direction, so no waiter
 * stays parked on a queue that holds something for it.
 *
 * This call does the same work as
 * ccol_select_timed(ready_index, n, selectables, UINT64_MAX).
 *
 * @param ready_index The function sets this to the index of the selectable
 *                    that became ready. It is valid only when the function
 *                    returns ccol_success.
 * @param n           Number of selectables, from 1 to INT_MAX. Internally the
 * library passes the index of the matched selectable through a plain int, so
 * a larger n is refused because it could truncate *ready_index. It must also
 * be at most SIZE_MAX / sizeof(waiter node), since this call keeps its
 * waiter nodes in one allocation of n nodes. No realistic caller gets near
 * either ceiling.
 * @param selectables Array of n ccol_selectable values to watch
 *
 * @return ccol_success           A selectable is ready, and *ready_index is
 *                                set. The caller must then do its own receive
 *                                or send. See above.
 * @return ccol_invalid_args      An argument is NULL or zero, n is too large
 *                                (see the n parameter above), a queue
 *                                selectable holds a NULL queue pointer, an
 *                                fd selectable has fd < 0, or the type or the
 *                                dir value is unknown.
 * @return ccol_not_enough_memory An allocation failed: the waiter nodes
 *                                (one node for each selectable), or, with
 *                                fd selectables, the state of the fds or
 *                                the event buffer of the wait. Any waiter
 *                                node that the call linked is unlinked
 *                                again, and no message moves.
 * @return ccol_unexpected_failure A system call failed: poll, epoll_create1,
 *                                eventfd, or epoll_ctl, or the
 *                                initialisation of the internal mutex or
 *                                condition variable. An fd selectable that
 *                                is not open, or that names a file that
 *                                epoll(7) cannot watch, gives this code on
 *                                every call; see the note below.
 *
 * @note If the array holds no fd selectable, the function uses only POSIX
 *       condition variables and makes no system call beyond those of a
 *       normal queue operation.
 * @note If the array holds at least one fd selectable, the function first
 *       asks poll(2), without waiting, which fds are ready, so a call that
 *       finds a ready selectable at once makes one poll call and nothing
 *       else for its fds. Only a call that has to wait uses epoll(7) and
 *       eventfd(2) internally on Linux, and kqueue(2) on the BSDs and macOS.
 *       That's why every fd must name a file that epoll(7) can watch, such
 *       as a socket, a pipe, a FIFO, a terminal or an eventfd. A regular
 *       file or a directory is refused with EPERM the way epoll_ctl refuses
 *       it, and the call returns ccol_unexpected_failure, where poll(2) and
 *       select(2) would report such a file as always ready.
 */
ccol_retval_t ccol_select(size_t *ready_index, size_t n,
                          ccol_selectable *selectables);

/**
 * @brief Wait with a timeout until one of n selectables becomes readable or
 * writable
 *
 * Does the same work as ccol_select(), except that the call returns
 * ccol_timed_out when no selectable becomes ready within timeout_us
 * microseconds.
 *
 * @param ready_index The function sets this to the index of the selectable
 *                    that became ready. It is valid when the function returns
 *                    ccol_success. The function does not change it on
 *                    ccol_timed_out, ccol_invalid_args,
 *                    ccol_not_enough_memory, or ccol_unexpected_failure.
 * @param n           Number of selectables. See the n parameter of
 *                    ccol_select for the exact bound.
 * @param selectables Array of n ccol_selectable values to watch
 * @param timeout_us  Maximum time to wait, in microseconds. UINT64_MAX waits
 *                    with no time limit, as ccol_select does. 0 polls
 *                    without blocking, and the poll still reports a
 *                    selectable (a queue or an fd) that is ready at the
 *                    moment of the call. Any other value waits at least that
 *                    long before the call reports a timeout, even when it's
 *                    longer than a single kernel wait can take (INT_MAX
 *                    milliseconds).
 *                    A value too large to form a deadline saturates to the
 *                    latest deadline the clock can hold, and never wraps
 *                    into the past.
 *
 * @return ccol_success           A selectable is ready, and *ready_index is
 *                                set.
 * @return ccol_timed_out         timeout_us ended and no selectable became
 *                                ready. The function does not change
 *                                *ready_index.
 * @return ccol_invalid_args      The same conditions as ccol_select
 * @return ccol_not_enough_memory The same conditions as ccol_select
 * @return ccol_unexpected_failure A system call failed, as with
 *                                ccol_select; or the array
 *                                holds no fd selectable, timeout_us is not
 *                                UINT64_MAX, and the timed wait on the
 *                                condition variable reported an unexpected
 *                                system error.
 *
 * @note The deadline uses CLOCK_MONOTONIC, so changing the system time
 *       doesn't change the timeout.
 * @note With no fd selectable in the array, the function uses
 *       pthread_cond_timedwait on a CLOCK_MONOTONIC condition variable. With
 *       at least one fd selectable, it passes the time that is left to each
 *       epoll_wait call, rounded up to a whole millisecond, and once the
 *       deadline has passed it looks at every selectable one more time, with
 *       a poll or an epoll_wait call with a timeout of 0, before it reports
 *       ccol_timed_out.
 */
ccol_retval_t ccol_select_timed(size_t *ready_index, size_t n,
                                ccol_selectable *selectables,
                                uint64_t timeout_us);

/**
 * @brief Short form: call ccol_select with the list of selectables in line
 *
 * Builds the array of selectables from its variadic arguments, computes the
 * count at compile time and hands everything to ccol_select. The macro's
 * result is the ccol_retval_t that ccol_select returns.
 *
 * Each argument must be a ccol_selectable value, usually built by
 * ccol_selectable_from_circq(), ccol_selectable_from_dynq(), or
 * ccol_selectable_from_chan(), with a dir of ccol_select_read or
 * ccol_select_write. Each argument is evaluated exactly once.
 *
 * Example:
 * @code
 *   size_t idx;
 *   ccol_retval_t r = ccol_select_va(&idx,
 *       ccol_selectable_from_circq(q0, ccol_select_read),
 *       ccol_selectable_from_chan(ch, ccol_select_read));
 *   if (r == ccol_success) {
 *       // Do the receive that fits the selectable that won.
 *       // For example, ccol_circq_try_recv_zc(q0, &msg).
 *   }
 * @endcode
 *
 * @note The macro uses a GCC and Clang statement expression, so it is not
 * valid under strict ISO C.
 */
#define ccol_select_va(ready_index, ...)                                   \
  __extension__({                                                          \
    ccol_selectable _cqsel_arr[] = {__VA_ARGS__};                          \
    ccol_select((ready_index), sizeof(_cqsel_arr) / sizeof(_cqsel_arr[0]), \
                _cqsel_arr);                                               \
  })

/**
 * @brief Short form: call ccol_select_timed with the list of selectables in
 * line
 *
 * Works like ccol_select_va(), but calls ccol_select_timed and passes it
 * timeout_us, in microseconds.
 *
 * Example:
 * @code
 *   size_t idx;
 *   ccol_retval_t r = ccol_select_timed_va(&idx, 500000,
 *       ccol_selectable_from_circq(q0, ccol_select_read),
 *       ccol_selectable_from_chan(ch, ccol_select_read));
 * @endcode
 *
 * @note The macro uses a GCC and Clang statement expression, so it is not
 * valid under strict ISO C.
 */
#define ccol_select_timed_va(ready_index, timeout_us, ...)                    \
  __extension__({                                                             \
    ccol_selectable _cqsel_arr[] = {__VA_ARGS__};                             \
    ccol_select_timed((ready_index),                                          \
                      sizeof(_cqsel_arr) / sizeof(_cqsel_arr[0]), _cqsel_arr, \
                      (timeout_us));                                          \
  })

/* ========================================================================== */
/*                             EVENT_LOOP API                                 */
/* ========================================================================== */

/**
 * @brief Opaque ccol_event_loop structure
 *
 * A long-lived reactor built on epoll(7) that the caller changes one step at
 * a time. In every configuration exactly ONE dedicated poller thread calls
 * epoll_wait, and that is part of the design, not an implementation detail:
 * when more than one thread calls epoll_wait on one shared epoll instance, a
 * single ready event wakes every blocked thread, a real kernel-level
 * thundering herd that costs measurable tail latency for a workload with few
 * concurrent operations. EPOLLEXCLUSIVE does NOT help here, which is easy to
 * miss, because it is about one target fd registered in several SEPARATE
 * epoll instances and not about many threads sharing one instance. See
 * num_reactor_threads on ccol_event_loop_create_mp for how the loop
 * spreads DISPATCH over many threads while only one thread polls.
 *
 * A registration is built from the same ccol_selectable type that ccol_select
 * uses (ccol_selectable_from_fd, ccol_selectable_from_circq,
 * ccol_selectable_from_dynq, ccol_selectable_from_chan). Like ccol_select,
 * ccol_event_loop never does the receive or the send itself for any type of
 * selectable, and the caller always makes its own call from inside the
 * callback: read(), recv(), ccol_circq_try_recv_zc(), or
 * ccol_dynmq_try_recv_zc(). See the documentation of ccol_event_loop_add.
 *
 * With num_reactor_threads > 1 the callbacks run on separate worker threads
 * instead of on the poller, and two correctness properties hold there that a
 * single-threaded reactor gets for free, because it has only one caller.
 * They are:
 *  - The loop never runs the callbacks of one registration concurrently with
 *    each other. It goes further than that: a read registration and a write
 *    registration that share an fd never run at the same time either, so a
 *    pair of callbacks that shares state across both directions of one fd,
 *    one TLS connection object for example, needs no lock of its own.
 *  - You can remove a registration and reuse its fd number for a new,
 *    unrelated registration even while a worker thread is inside a dispatch
 *    that points to the old one. A dispatch always checks liveness right
 *    before it calls a callback, so a stale pointer to a removed
 *    registration is a safe no-op: never a use-after-free, and the loop
 *    never sends that callback to the new registration. For the same reason
 *    the loop never collects a second registration for the same entry while
 *    an earlier one is in flight (queued or running). Application code can
 *    therefore call ccol_event_loop_modify from inside a callback that is in
 *    flight, a supported and common pattern, without racing a second
 *    dispatch that the loop collects at the same moment for that same
 *    registration.
 *
 * A third property holds in every configuration, whatever num_reactor_threads
 * is. Different threads can call ccol_event_loop_modify,
 * ccol_event_loop_pause, ccol_event_loop_resume, ccol_event_loop_remove, and
 * ccol_event_loop_reg_generation at the same time on the very same
 * ccol_event_reg, for example one thread removing a registration while
 * another races it and tries to change it or ask about it. In fact a thread
 * can make such a call at any time at all, however long ago another thread
 * called ccol_event_loop_remove() on that registration. Every use of a
 * ccol_event_reg handle goes through the loop's generation-tagged slot table
 * before the library dereferences anything (see the doc comment of
 * ccol_event_reg), so a stale or already removed handle is detected every
 * time and is never a use-after-free. The side that loses the race just sees
 * a registration that is already removed and gets ccol_invalid_args, or
 * generation 0. See ccol_event_loop_reg_generation() for the identity token
 * this gives the caller, which a registration can use for its own defensive
 * bookkeeping across the reuse of an fd. That's a separate concern from the
 * two guarantees above, which always hold whether or not a caller ever reads
 * the token.
 */
typedef struct _ccol_event_loop_s _ccol_event_loop_s;

/**
 * @brief Opaque ccol_event_loop handle.
 *
 * ccol_event_loop is an opaque VALUE handle that packs a slot index and a
 * generation into one value. It is not a pointer: never cast it to or from
 * void*, never compare it through a pointer cast, and never treat it as an
 * address. Compare it directly against CCOL_EVENT_LOOP_NULL, or use it in
 * a truthiness check, since CCOL_EVENT_LOOP_NULL is 0 and `if (!loop)` is
 * a correct test for "no loop". The library resolves every use of a
 * ccol_event_loop through a slot table it owns before it touches the struct
 * _ccol_event_loop_s* behind the handle, so it always detects a handle whose
 * slot is free or holds an unrelated, later loop, and it never dereferences
 * freed memory or the memory of the wrong object. See the doc comment of
 * ccol_event_loop_destroy for what that function does with a stale handle.
 */
typedef uint64_t ccol_event_loop;

/** @brief Sentinel value for "no loop", the ccol_event_loop equivalent of
 * NULL.
 */
#define CCOL_EVENT_LOOP_NULL ((ccol_event_loop)0)

/**
 * @brief Opaque handle to a single ccol_event_loop registration.
 *
 * ccol_event_reg is an opaque VALUE handle that packs a slot index and a
 * generation into one value, and it belongs to the one ccol_event_loop that
 * gave it out. It is not a pointer: never cast it to or from void*, never
 * compare it through a pointer cast, and never treat it as an address.
 * Compare it directly against CCOL_EVENT_REG_NULL, or use it in a
 * truthiness check, since CCOL_EVENT_REG_NULL is 0. The library resolves
 * every use of a ccol_event_reg through that loop's reg slot table before it
 * touches the struct behind the handle, so it always detects a handle whose
 * slot is free. The slot becomes free when ccol_event_loop_remove runs, from
 * this thread or from another thread racing it, and the matching entry point
 * then returns ccol_invalid_args (0 for ccol_event_loop_reg_generation)
 * instead of a use-after-free. It's the same handle design as
 * ccol_event_loop, scoped to one loop instead of the whole process.
 */
typedef uint64_t ccol_event_reg;

/** @brief Sentinel value for "no registration", the ccol_event_reg
 * equivalent of NULL. */
#define CCOL_EVENT_REG_NULL ((ccol_event_reg)0)

/**
 * @brief Callback that the loop calls when a registration becomes readable
 *
 * For every type of selectable the callback only reports readiness, and the
 * reactor never does the receive itself. For an fd selectable the callback
 * makes its own read() or recv() call on sel->fd. For a queue selectable (a
 * circq, a dynq, or a queue that comes from a channel) it makes its own
 * ccol_circq_try_recv_zc() or ccol_dynmq_try_recv_zc() call on sel->cq or
 * sel->dq, and for a queue from a channel that means ccol_circq_try_recv_zc()
 * on sel->cq, never ccol_chan_try_recv_zc() (see ccol_selectable_from_chan).
 * In both cases another consumer may already have taken the data. That
 * TOCTOU race is part of the design and the same contract as the write
 * direction of ccol_select, so the callback's receive can find nothing and
 * the callback must cope with that.
 *
 * For a queue selectable one notification doesn't always mean exactly one
 * message: several sends can outrun the dispatch and the callback then sees
 * them as a single notification. A callback that receives one message per
 * call still drains a burst, because as long as the queue stays ready after a
 * callback that moved at least one message the loop dispatches the listeners
 * again. A callback can also receive in a loop until the call returns
 * ccol_container_empty, which drains a burst in fewer dispatches.
 *
 * Every callback of the three dispatch types (this one,
 * ccol_event_writable_fn and ccol_event_error_fn) gets the handle of its own
 * registration in reg. That's exactly the value ccol_event_loop_add returns
 * for the registration, and the callback can pass it to
 * ccol_event_loop_remove, ccol_event_loop_pause, ccol_event_loop_resume,
 * ccol_event_loop_modify and ccol_event_loop_reg_generation. It holds even
 * for a dispatch that runs before ccol_event_loop_add has returned to its
 * caller, which can happen with num_reactor_threads > 1 when the selectable
 * is already ready at registration time, so a callback never has to read the
 * handle from storage that the adding thread writes after the call returns.
 *
 * @param loop The ccol_event_loop that owns this registration
 * @param reg  The handle of this registration, as ccol_event_loop_add returns
 *             it
 * @param sel  The ccol_selectable this registration comes from. sel->dir is
 *             the registration's current direction, which a
 *             ccol_event_loop_modify call can change after
 *             ccol_event_loop_add.
 * @param arg  The opaque pointer that the caller gave to ccol_event_loop_add
 */
typedef void (*ccol_event_readable_fn)(ccol_event_loop loop, ccol_event_reg reg,
                                       ccol_selectable *sel, void *arg);

/**
 * @brief Callback that the loop calls when a registration becomes writable
 *
 * The callback never gets a payload. For an fd selectable the caller makes
 * its own write() or send() call. For a queue selectable the registration
 * only signals that the queue may have room or that its sends are turned off
 * (the send then returns ccol_not_permitted), and the callback has to call
 * ccol_circq_try_send_zc or ccol_dynmq_send_zc itself; for a queue from a
 * channel that's ccol_circq_try_send_zc on sel->cq, never
 * ccol_chan_try_send_zc (see ccol_selectable_from_chan). A TOCTOU race is
 * possible, the same contract as a win on the write direction of
 * ccol_select.
 *
 * @param loop The ccol_event_loop that owns this registration
 * @param reg  The handle of this registration; see ccol_event_readable_fn
 * @param sel  The ccol_selectable that this registration comes from
 * @param arg  The opaque pointer that the caller gave to ccol_event_loop_add
 */
typedef void (*ccol_event_writable_fn)(ccol_event_loop loop, ccol_event_reg reg,
                                       ccol_selectable *sel, void *arg);

/**
 * @brief Callback that the loop calls on a fatal condition of a registration
 *
 * For an fd selectable this callback runs for every condition the
 * registration's direction has no other handler for. EPOLLERR and EPOLLHUP
 * come here whenever the direction isn't also ready, and for a
 * read-direction registration with on_readable == NULL so does the close or
 * shutdown of the peer's end, which is EPOLLRDHUP (an ordinary TCP close()
 * raises it without EPOLLHUP).
 *
 * If the direction does have its own handler, that handler takes the event
 * whenever the direction is ready as well. Data can arrive together with the
 * error, and a read-direction registration with on_readable set then gets
 * on_readable instead of on_error, so the bytes that arrived before the peer
 * went away are delivered. A write-direction registration with on_writable
 * set gets on_writable for the same reason. When both directions are
 * registered on one fd the loop judges each direction on its own, and each
 * can get an error dispatch. A queue selectable never produces an error
 * condition and never calls this callback.
 *
 * A registration with on_error == NULL gets EPOLLERR and EPOLLHUP through its
 * own direction's handler instead, the way libevent and libuv deliver them:
 * on_readable for a read registration, on_writable for a write registration.
 * That handler's read(), recv(), write() or send() then reports the
 * condition, -1 with the error in errno or 0 for an end of file. A read or a
 * write on a socket also clears a pending socket error, so a transient error
 * (the ICMP port unreachable that a connected UDP socket reports as
 * ECONNREFUSED, say) costs one failed receive, and the datagrams arriving
 * after it reach on_readable as usual. When both directions of one fd have
 * such a handler, one event reaches both: on_readable first and then
 * on_writable, one after the other on the same thread, and each sees
 * whatever the other left of the condition.
 *
 * @note A registration can carry on_error alone and leave the on_readable or
 * on_writable of its own direction NULL. That's how a caller says "tell me
 * when this fd dies, I don't want to read it or write to it". Both
 * directions fully support this shape, and on a socket, a pipe, a FIFO or a
 * terminal the loop never arms such a registration for the readiness it has
 * no handler for; on either end of a pipe the condition it reports is the
 * close of the other end. A healthy socket is writable from the moment it is
 * registered, so an error-only registration on the write direction costs
 * nothing until something goes wrong.
 *
 * @warning An error is level-triggered like every other condition here: the
 * loop keeps reporting it and keeps calling on_error (or the direction's
 * handler when on_error is NULL) until a call removes or pauses the
 * registration, or until the condition clears. A hang-up stays true, so a
 * pipe whose writer closed or a socket whose peer is gone keeps reporting
 * it, and a read() keeps returning 0. A handler that neither removes the
 * registration nor clears the condition some other way is therefore called
 * many times. A handler that reads an end of file must remove the
 * registration (or pause it), exactly as for any other readiness it leaves
 * in place. Remove it before you close the fd; see ccol_event_loop_remove.
 * On the BSDs and macOS an error-only registration may hear each error or
 * hang-up only once, when it happens, instead of on every wait, because the
 * kernel filter watching it there can be edge-triggered; a handler that
 * removes or pauses the registration behaves the same on every system.
 *
 * @param loop The ccol_event_loop that owns this registration
 * @param reg  The handle of this registration; see ccol_event_readable_fn
 * @param sel  The ccol_selectable that this registration comes from
 * @param arg  The opaque pointer that the caller gave to ccol_event_loop_add
 */
typedef void (*ccol_event_error_fn)(ccol_event_loop loop, ccol_event_reg reg,
                                    ccol_selectable *sel, void *arg);

/**
 * @brief Callback that the loop calls when it can prove that the caller can
 * free arg
 *
 * The loop calls this callback exactly once for every registration it really
 * put into an ccol_event_loop, and never for a ccol_event_loop_add call that
 * failed. It happens on one of two occasions. The first is when a thread
 * calls ccol_event_loop_remove() for this registration (any thread, even one
 * inside this same registration's on_readable, on_writable, or on_error
 * callback) and then every dispatch of the registration that was in flight,
 * or that the loop had already collected for dispatch at that moment,
 * finishes. The second is a destroy of the owning ccol_event_loop while this
 * registration is live.
 *
 * Of the four callback types this is the only asynchronous one: the loop
 * never calls it from inside ccol_event_loop_remove(), and it can run a while
 * after that call returns. The delay is bounded, and with
 * num_reactor_threads == 1 it runs at the next point between two batches on
 * the reactor thread. See the note of ccol_event_loop_remove() for why the
 * caller can't free arg's memory as soon as the removal returns.
 * ccol_event_loop_destroy() works differently: before it returns it calls,
 * from inside the call, every on_removed that is still due, which covers a
 * registration live at the destroy and a removed one whose on_removed hasn't
 * run yet.
 *
 * The callback runs on the thread doing the reclamation, which is the
 * reactor thread for an ordinary removal and the thread that called
 * ccol_event_loop_destroy for a registration live at a destroy. That thread
 * holds none of this module's internal locks, so on_removed can always take
 * an application lock of its own, even one that the other callbacks of this
 * registration take too.
 *
 * Calling ccol_event_loop_destroy() on the owning loop from on_removed is
 * fatal (_ccol_fatal_err()). On an ordinary removal on_removed runs on the
 * reactor thread, where a destroy of its own loop is refused, and during a
 * destroy the handle is already destroyed.
 *
 * A registration can set on_removed to NULL and then gets the same contract
 * as every other callback type: nothing is promised about when a stale
 * callback still in flight is done with arg, beyond the weaker promise in the
 * documentation of ccol_event_loop_remove(), which is weaker by design.
 *
 * @param arg The opaque pointer that the caller gave to ccol_event_loop_add
 */
typedef void (*ccol_event_removed_fn)(void *arg);

/**
 * @brief Group of callbacks for one ccol_event_loop_add call
 *
 * Each of on_readable, on_writable, and on_error can be NULL. For an fd
 * selectable the loop doesn't arm a direction that has no handler of its
 * own, so a NULL on_readable or on_writable costs no dispatch at all, since
 * the loop never makes a dispatch that nothing can use. A condition that
 * reaches such a direction, like the peer closing or an error, goes to
 * on_error, and with on_error NULL an error or a hang-up goes to the
 * direction's handler instead (see ccol_event_error_fn). A producer that only
 * writes, for example, can pass NULL for on_error and see an error as its
 * own write() failing in on_writable.
 *
 * An fd event that none of the registration's handlers can take is dropped,
 * and the loop also stops watching that registration, because a hang-up can
 * stay true and a level-triggered epoll would report it again on every
 * wait. Only a registration with neither its direction's handler nor
 * on_error ends up there, for example a read registration with both
 * on_readable and on_error NULL whose peer hangs up. The registration stays
 * registered, and a registration for the other direction of the same fd is
 * still watched. ccol_event_loop_resume() and a successful
 * ccol_event_loop_modify() arm it again, whether the modify changes the
 * direction or keeps it.
 *
 * on_removed can be NULL too, because some args need no notice at all before
 * the caller frees them, an arg on the stack or one the program owns
 * statically for example. A caller may also have its own way of knowing when
 * a free is safe, for instance the registration can destroy arg well after
 * ccol_event_loop_remove() returns instead of at the moment it returns.
 */
typedef struct ccol_event_handlers {
  ccol_event_readable_fn
      on_readable; /**< EPOLLIN|EPOLLRDHUP, or a queue message */
  ccol_event_writable_fn on_writable; /**< EPOLLOUT, or room in the queue */
  ccol_event_error_fn on_error; /**< EPOLLERR|EPOLLHUP (fd selectables only) */
  ccol_event_removed_fn on_removed; /**< Runs when nothing points to arg */
} ccol_event_handlers_t;

/**
 * @brief Create a ccol_event_loop with custom memory management
 *
 * Creates a long-lived epoll instance and starts the threads that drive it
 * at once, so every thread is ready to dispatch events as soon as this
 * call returns. It works like ctpool_create in that respect: the pool is
 * ready to work the moment you get the handle.
 *
 * num_reactor_threads == 1 gives a pure single-thread reactor, where that one
 * thread calls epoll_wait AND runs every callback in line.
 * num_reactor_threads > 1 starts exactly ONE dedicated thread that calls
 * epoll_wait, never more (the doc comment of the ccol_event_loop struct
 * explains why), plus (num_reactor_threads - 1) worker threads that run the
 * callbacks. So the total OS thread count for a given num_reactor_threads is
 * always exactly that value, and the parameter means the same thing for
 * resource use in both configurations.
 *
 * In the first configuration a registration's callback runs on the poller
 * thread, and in the second on one of the worker threads. Callback code
 * can't tell the difference, since ccol_event_readable_fn,
 * ccol_event_writable_fn, and ccol_event_error_fn have no way to read it.
 * One rule holds for both kinds of thread: do not call
 * ccol_event_loop_shutdown from inside a callback, because that's a
 * self-join hazard. See the doc comment of ccol_event_loop_shutdown.
 *
 * num_reactor_threads == 1 costs exactly what a plain single-threaded loop
 * costs, because the loop never creates the dispatch pool and the code path
 * is the same. Above one thread, a single dedicated poller is what keeps tail
 * latency low when few operations run together, while N threads all calling
 * epoll_wait on the same set of fds make a real kernel thundering herd. That
 * comes from the kernel, not from this module's code; see the doc comment of
 * the ccol_event_loop struct. The separate dispatch pool keeps the overall
 * multi-threaded dispatch throughput up under real concurrent load.
 *
 * The registry of fds and entries is lock-striped: it holds num_lock_stripes
 * independent pairs of a mutex and a chmap, and each pair guards its own
 * subset of the registrations, with no overlap between subsets. One real fd,
 * or the private bridge fd of one queue registration or one ccol_channel
 * registration, is handled by exactly one stripe and is never split across
 * two. A value of 1 gives one registry lock for the whole loop and costs only
 * one more step through an array. A larger value lets ccol_event_loop_add,
 * ccol_event_loop_remove, and ccol_event_loop_modify calls for different fds
 * and registrations run in parallel instead of queueing on one lock, at the
 * cost of num_lock_stripes mutexes and chmaps that the loop allocates up
 * front. This is independent of num_reactor_threads: the stripe count
 * controls contention on the registry, and the thread count controls how
 * much dispatch runs in parallel.
 *
 * @param max_events_per_wait Size of the poller thread's epoll_wait batch
 * buffer. It must be from 1 to INT_MAX and small enough that
 * max_events_per_wait * sizeof(struct epoll_event) doesn't overflow size_t;
 * for a value outside that range this function returns
 * CCOL_EVENT_LOOP_NULL. The
 * library narrows the value to epoll_wait's int maxevents parameter and sizes
 * the poller thread's events buffer from it. It bounds how many ready events
 * one epoll_wait call drains, not how many registrations the loop can hold.
 * @param num_lock_stripes Number of independent lock stripes for the registry
 * of fds and entries. It must be >= 1, and 1 means one registry lock for the
 * whole loop. A larger value lowers the contention of ccol_event_loop_add,
 * ccol_event_loop_remove, and ccol_event_loop_modify across many different
 * fds and registrations under concurrent use. There is no upper bound.
 * @param num_reactor_threads Total number of OS threads that poll and
 * dispatch for this loop. It must be >= 1. A value of 1 means a pure
 * single-thread reactor where one thread polls and dispatches in line, and
 * any larger value means exactly one dedicated polling thread plus
 * (num_reactor_threads - 1) dispatch worker threads. See the doc comment of
 * the ccol_event_loop struct for the two correctness guarantees, which hold
 * for every value.
 * @param mmgmt_procs Custom memory management procedures, or NULL to use the
 * default malloc and free
 * @param err_str Optional pointer that gets an error string on failure. Pass
 * NULL to ignore it.
 *
 * @return New ccol_event_loop handle, or CCOL_EVENT_LOOP_NULL on failure.
 * The call fails on invalid arguments, on a failed allocation, and when
 * epoll_create1, eventfd, or pthread_create fails. It also fails when the
 * process has no thread-specific key left for the two keys that every loop's
 * dispatch uses (the library creates them once per process and doesn't try
 * again), and once the library's process-exit destructors have run and the
 * last loop is gone, for example when it is called from an application
 * destructor that runs after them.
 *
 * @see ccol_event_loop_create
 * @see ccol_event_loop_destroy
 */
ccol_event_loop ccol_event_loop_create_mp(size_t max_events_per_wait,
                                          size_t num_lock_stripes,
                                          size_t num_reactor_threads,
                                          ccol_memmgmt_procs_t *mmgmt_procs,
                                          char **err_str);

/**
 * @brief Create a ccol_event_loop with default memory management
 *
 * Short form of ccol_event_loop_create_mp with mmgmt_procs = NULL.
 */
#define ccol_event_loop_create(max_events_per_wait, num_lock_stripes,  \
                               num_reactor_threads, err_str)           \
  ccol_event_loop_create_mp((max_events_per_wait), (num_lock_stripes), \
                            (num_reactor_threads), NULL, (err_str))

/**
 * @brief Register a selectable with the event loop
 *
 * Takes the same ccol_selectable type as ccol_select, so you can build sel
 * directly with ccol_selectable_from_fd, ccol_selectable_from_circq,
 * ccol_selectable_from_dynq, or ccol_selectable_from_chan.
 *
 * One registration covers exactly one direction, sel.dir. A caller that
 * wants both directions live on one fd at the same time, like a full-duplex
 * pipe, calls ccol_event_loop_add twice and gets two independent handles. A
 * caller that wants one registration to change direction over time, like a
 * connecting socket (write interest until the connect finishes, read
 * interest after that), uses one registration and ccol_event_loop_modify.
 * The function refuses a second selectable for a direction already in use on
 * the same fd, such as two read registrations on one fd; the call then
 * returns CCOL_EVENT_REG_NULL and err_str names the cause (see the return
 * value below).
 *
 * Queue and ccol_channel selectables have no such restriction. You can make
 * any number of ccol_event_loop_add calls for the same queue and direction,
 * in one ccol_event_loop or in several, and mix them with a ccol_select()
 * call running at the same time on the same queue and direction. All of them
 * are live together and each gets its turn, the same guarantee ccol_select
 * gives: any number of threads can watch one queue at the same time.
 *
 * When a message arrives while several such listeners are registered, the
 * library wakes exactly one listener per producer call and per consumer
 * call, never all of them at once, since that would be a thundering herd.
 * When the woken listener's callback returns, the listener checks whether
 * the queue is still ready for its direction and, if it is, hands the wake on
 * to the next listener in line. A ccol_select() caller that wakes and leaves
 * without acting on the queue hands the wake on the same way, and so does a
 * registration that is removed, or whose loop is shut down or destroyed,
 * before it acted on its wake. A registration of a loop that is shut down
 * takes no further wakes. So no wake is lost, and the library never skips a
 * live listener for ever.
 *
 * A registration whose callback moved at least one message is dispatched
 * again for as long as the queue stays ready for its direction (for a read
 * registration the queue holds fewer messages after the callback than
 * before, and for a write registration it holds more). This holds for each
 * registration on its own, wherever it sits in the line and whatever the
 * queue's other listeners do, so a backlog drains without another send or
 * receive even when each callback receives one message, and a listener that
 * declines the queue never holds up one that takes it.
 * A registration whose callback neither receives nor sends hands the wake
 * on, and a wake that every listener declines stops once it has visited each
 * of them, so listeners that ignore a ready queue don't spin. A write
 * registration whose callback sends on every call is dispatched again for as
 * long as the queue has room, exactly like a writable fd, so remove it when
 * it has nothing to send. A queue whose sends are turned off is ready for a
 * write registration. Turning sends off wakes the registration once, and a
 * registration added while sends are off is woken once as soon as it's
 * added. Its send then returns ccol_not_permitted and moves nothing, so the
 * wake is handed on as described above, and the registration is woken again
 * only by a later event on the queue, such as a receive or another change of
 * the send state.
 *
 * The registration is live, and its callbacks can run, before this call
 * returns: with num_reactor_threads > 1 a callback can run on a dispatch
 * worker while the calling thread is still inside this function, for example
 * when the fd is already ready. Each callback gets its own registration's
 * handle as its reg parameter, so it never has to read this call's return
 * value from storage that the calling thread writes afterwards.
 *
 * @param loop     ccol_event_loop to register with
 * @param sel      What to watch; see above.
 * @param handlers Group of callbacks, each of which can be NULL.
 * @param arg      Opaque pointer the loop passes to every callback of this
 *                 registration
 * @param err_str  Optional pointer that gets an error string on failure, and
 *                 NULL on success
 *
 * @return New registration handle, or CCOL_EVENT_REG_NULL on failure. On
 * failure, when err_str is not NULL, *err_str names the cause with one
 * message per class. A loop that is shut down (ccol_event_loop_shutdown())
 * refuses every registration with a message containing "shut down", because
 * nothing in it dispatches any more. A direction that another registration on
 * the same fd already holds gives a message containing
 * "direction already in use";
 * no retry can succeed there. A failed allocation gives a message containing
 * "failed to allocate", and a failed system call (epoll_ctl, eventfd, or the
 * initialisation of a mutex or a condition variable) gives one containing
 * "a system call failed". An fd naming a regular file or a directory always
 * fails in that last way, since epoll(7) can't watch such a file (epoll_ctl
 * fails with EPERM). An invalid or stale loop handle and malformed arguments
 * have messages of their own.
 *
 * @note The function is thread-safe: you can call it at the same time as
 *       ccol_event_loop_remove and ccol_event_loop_modify, and also from
 *       inside a callback that runs on the reactor thread.
 *
 * @see ccol_event_loop_remove
 * @see ccol_event_loop_modify
 */
ccol_event_reg ccol_event_loop_add(ccol_event_loop loop, ccol_selectable sel,
                                   ccol_event_handlers_t handlers, void *arg,
                                   char **err_str);

/**
 * @brief Identity token that the caller can read for the fd below a
 * registration
 *
 * The value only goes up and is unique across the whole loop. The new-entry
 * path of ccol_event_loop_add makes it once, when the library first registers
 * the fd that reg belongs to (which can also be the bridge fd of a queue or a
 * ccol_channel), and every registration on that fd shares it for as long as
 * the fd lives. It survives a ccol_event_loop_modify call, because a change
 * of direction keeps the same fd and the same connection, and a read and a
 * write registration on one fd share one generation as well, since they are
 * one logical connection.
 *
 * The token exists for a caller's defensive bookkeeping across the reuse of
 * an fd. A caller holding a connection object across several asynchronous
 * steps can stamp it with the generation it reads right after
 * ccol_event_loop_add returns, read the generation again later, and compare
 * the two to see whether it is still working with the same logical
 * connection. Correctness doesn't depend on the token, because the
 * ccol_event_loop dispatch checks a registration's liveness before calling
 * any callback, every time, whether or not a caller ever calls this
 * function. See the doc comment of the ccol_event_loop struct.
 *
 * You can call this function at any time on any ccol_event_reg value that
 * ccol_event_loop_add ever gave out for this loop, including one that has
 * already been removed, whether by an earlier call on this thread or by a
 * call on another thread racing this one right now. reg is resolved through
 * the loop's generation-tagged slot table before anything is dereferenced,
 * so a stale or already removed handle is always detected and reports
 * generation 0, never a use-after-free.
 *
 * @param loop ccol_event_loop that owns reg
 * @param reg Registration to ask about
 * @return The generation value, or 0. The function gives 0 when reg is
 * CCOL_EVENT_REG_NULL, when reg has already been removed, when loop is
 * CCOL_EVENT_LOOP_NULL, and when loop is stale or already destroyed. The
 * counter starts at 1, so 0 is never a valid generation for a real
 * registration.
 */
uint64_t ccol_event_loop_reg_generation(ccol_event_loop loop,
                                        ccol_event_reg reg);

/**
 * @brief Change the direction of an fd registration that already exists
 *
 * Works only on an fd. A registration that comes from a queue selectable or
 * a ccol_channel selectable gets ccol_invalid_args, because a queue
 * selectable's direction is part of its identity; remove such a registration
 * and add it again instead.
 *
 * On success the function changes the registration's ccol_selectable.dir,
 * and later callbacks see the new value in their sel parameter. It moves the
 * registration to the other slot of its fd and computes the fd's combined
 * epoll interest mask again, with no moment where the fd isn't registered. A
 * reg the loop stopped watching because it got an event none of its
 * handlers could take (see ccol_event_handlers_t) is watched again in its new
 * direction. A call whose new_dir is reg's current direction changes nothing
 * else, but it does make such a reg watched again.
 *
 * @param loop    ccol_event_loop that owns the registration
 * @param reg     Registration to change
 * @param new_dir ccol_select_read or ccol_select_write
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop is CCOL_EVENT_LOOP_NULL, a stale
 * handle, or a handle that a call already destroyed; if reg is
 * CCOL_EVENT_REG_NULL, or does not resolve for another reason (a call on
 * this thread or on another thread that races it can already have removed
 * reg); if reg is a queue registration or a ccol_channel registration; or if
 * new_dir is not valid.
 * @return ccol_not_permitted if another registration on the same fd already
 * holds the target direction
 *
 * @note The function is thread-safe: you can call it at the same time as
 *       ccol_event_loop_remove, and also from inside a callback that runs on
 *       the reactor thread.
 */
ccol_retval_t ccol_event_loop_modify(ccol_event_loop loop, ccol_event_reg reg,
                                     ccol_select_dir new_dir);

/**
 * @brief Stop the delivery of events for an fd registration for a time, and
 *        keep the registration
 *
 * Works only on an fd, with the same restriction as ccol_event_loop_modify.
 * ccol_event_loop_remove unregisters reg completely and defers the free,
 * while ccol_event_loop_pause keeps reg whole: the registration keeps its
 * slot on the fd's entry, counts in ccol_event_loop_reg_count, and keeps the
 * same ccol_event_loop_reg_generation. The function only recomputes the fd's
 * combined epoll interest mask and leaves reg out of it. While reg is paused
 * no on_readable, on_writable, or on_error callback runs for it, the same as
 * after a removal. The other direction on the same fd, if there is one, is
 * not affected.
 *
 * For callers that bring the same logical registration back later, this is
 * the cheap replacement for a ccol_event_loop_remove followed later by a
 * ccol_event_loop_add, for example a connection that goes to a worker thread
 * for blocking body I/O and then comes back to the reactor for its next
 * request. The pause needs no heap allocation and no free, and causes no
 * churn in the fd registry's chmap. With one reactor thread
 * (num_reactor_threads == 1) it costs one epoll_ctl call, where a
 * remove-then-add pair costs two, DEL and then ADD.
 *
 * With more than one reactor thread the dispatch worker re-arms
 * EPOLLONESHOT once more after a paused callback returns. That's harmless:
 * ccol_event_loop's internal re-arm helper always recomputes the mask from
 * live state, so the extra re-arm applies the same mask (which leaves reg
 * out) and brings back no race. Pause and resume avoid the allocation and the
 * registry churn in that configuration too.
 *
 * Pausing a reg that is already paused succeeds and does nothing.
 *
 * @param loop ccol_event_loop that owns the registration
 * @param reg  Registration to pause
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop is CCOL_EVENT_LOOP_NULL, a stale
 * handle, or a handle that a call already destroyed; if reg is
 * CCOL_EVENT_REG_NULL, or does not resolve for another reason (a call on
 * this thread or on another thread that races it can already have removed
 * reg); or if reg is a queue registration or a ccol_channel registration.
 *
 * @note The function is thread-safe: you can call it at the same time as
 *       ccol_event_loop_remove, and also from inside a callback that runs on
 *       the reactor thread.
 *
 * @see ccol_event_loop_resume
 * @see ccol_event_loop_remove
 */
ccol_retval_t ccol_event_loop_pause(ccol_event_loop loop, ccol_event_reg reg);

/**
 * @brief Start the delivery of events again for a registration that
 *        ccol_event_loop_pause paused
 *
 * Recomputes the fd's combined epoll interest mask and puts reg back into
 * it. It also re-arms a reg that the loop stopped watching because it got an
 * event none of its handlers could take (see ccol_event_handlers_t). Resuming
 * a reg that is neither paused nor stopped that way, such as one that was
 * never paused or one already resumed, succeeds and does nothing, which is
 * the same rule as in ccol_event_loop_modify: a call that asks for the state
 * the object already has does nothing and succeeds.
 *
 * @param loop ccol_event_loop that owns the registration
 * @param reg  Registration to resume
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop is CCOL_EVENT_LOOP_NULL, a stale
 * handle, or a handle that a call already destroyed; if reg is
 * CCOL_EVENT_REG_NULL, or does not resolve for another reason; or if reg
 * is a queue registration or a ccol_channel registration. This can happen,
 * for example, when the connection closes while the caller still believes it
 * owns a paused registration to resume.
 *
 * @note The function is thread-safe: you can call it at the same time as
 *       ccol_event_loop_remove, and also from inside a callback that runs on
 *       the reactor thread.
 *
 * @see ccol_event_loop_pause
 */
ccol_retval_t ccol_event_loop_resume(ccol_event_loop loop, ccol_event_reg reg);

/**
 * @brief Remove the registration of a selectable from the event loop
 *
 * You can call this function from inside a callback of the very registration
 * you are removing (a registration that removes itself on an error is a
 * common pattern), from any other thread, and even while a dispatch for the
 * same registration is in flight. Once this call returns the loop never
 * calls a callback for this registration again, including one it has already
 * collected but not started; with num_reactor_threads > 1 such a callback is
 * sitting in a dispatch worker thread's queue.
 *
 * This call does not wait, though, for a dispatch that is in flight when it
 * is made. A caller that must not free the memory arg points to while that
 * dispatch still runs (the usual case for an arg that isn't fully
 * self-contained, such as one a callback dereferences) needs the on_removed
 * callback of ccol_event_handlers_t, set in the ccol_event_loop_add call,
 * which runs asynchronously at the moment the free is provably safe.
 * ccol_event_loop_remove itself never blocks on on_removed or on that
 * in-flight dispatch, because blocking there would risk a lock order cycle:
 * the dispatch's callback can need an application lock while the caller of
 * this function holds a different one.
 *
 * A caller that is sure no callback can be in flight at the moment of the
 * removal, for example when it removes from inside that exact
 * registration's callback or the loop never dispatched the registration, can
 * free arg immediately and doesn't need on_removed at all.
 *
 * The function doesn't close an fd and doesn't change a queue's lifetime. It
 * frees only the ccol_event_loop's own registration bookkeeping, in line
 * with the contract of ccol_selectable_from_fd, under which the caller owns
 * the fd.
 *
 * Remove every registration of an fd before you close the fd, which libevent
 * and libuv require too. Once the last of them is removed the loop has
 * already stopped watching the fd by the time this call returns, and the fd
 * number is free: a new registration of the same fd, or of a new descriptor
 * reusing the number, is independent of the removed one, and nothing the
 * loop still does for the removed registration (a dispatch in flight, or the
 * later release of its bookkeeping) changes how the new one is watched. A
 * registration whose fd was closed first holds the number until it is
 * removed, and meanwhile a ccol_event_loop_add of a descriptor that reuses
 * the number fails with ccol_not_permitted for the direction the stale
 * registration holds, and can fail with ccol_unexpected_failure for the
 * other direction.
 *
 * For a queue selectable or a ccol_channel selectable the loop finishes its
 * own bookkeeping for that queue before this call returns: it unlinks the
 * registration's waiter from the queue and frees the registration
 * bookkeeping. A dispatch in flight is a separate matter, because its
 * callback holds the selectable the loop gave it and reaches the queue
 * through it. Unlike for arg above, the caller needs no on_removed handshake
 * for the queue, since destroying the queue waits for any such callback to
 * finish with it; see ccol_circular_queue_destroy. The ordinary sequence
 * (drain, remove, destroy) is therefore safe from any thread that doesn't
 * itself run one of this registration's callbacks, a callback of another
 * registration on the same loop included.
 *
 * @param loop ccol_event_loop that owns the registration
 * @param reg  Registration to remove
 *
 * You can call this function more than once on the same reg, either one call
 * after the other or with calls that really run at the same time on
 * different threads. Neither is ever a use-after-free, but they return
 * different values.
 *
 * A call that starts strictly after an earlier ccol_event_loop_remove() on
 * the same reg has returned doesn't resolve reg_h at all, because the
 * earlier call freed reg's slot before returning, so it gets
 * ccol_invalid_args. That's the same result that
 * ccol_event_loop_modify(), ccol_event_loop_pause(), ccol_event_loop_resume(),
 * and ccol_event_loop_reg_generation() document for an already removed reg.
 *
 * A call that really races another ccol_event_loop_remove() on the same reg
 * is different: its own resolve can succeed because the other call hasn't
 * freed reg's slot yet. In that narrow window it sees that reg already
 * carries the removed mark, does nothing, and returns ccol_success.
 *
 * @return ccol_success on success, including the call that loses the race
 * in the window above.
 * @return ccol_invalid_args if loop is CCOL_EVENT_LOOP_NULL, a stale
 * handle, or a handle that a call already destroyed; or if reg is
 * CCOL_EVENT_REG_NULL, or does not resolve for another reason, for example
 * because an earlier call that already returned removed it.
 *
 * @note The function is thread-safe
 *
 * @see ccol_event_removed_fn
 */
ccol_retval_t ccol_event_loop_remove(ccol_event_loop loop, ccol_event_reg reg);

/**
 * @brief Number of ccol_event_reg handles that are registered now
 *
 * Counts live registrations, meaning those from a ccol_event_loop_add call
 * that haven't been removed yet, not the entries of the epoll interest list,
 * so one fd with both directions registered counts as 2.
 *
 * @param loop ccol_event_loop to ask
 * @return Count of registrations, or (size_t)-1 if loop is
 * CCOL_EVENT_LOOP_NULL, a stale handle, or a handle that a call already
 * destroyed.
 */
size_t ccol_event_loop_reg_count(ccol_event_loop loop);

/**
 * @brief Stop the poller thread of the reactor and its dispatch worker
 *        threads, if it has any
 *
 * You can call this function more than once, or not at all before
 * ccol_event_loop_destroy, since that macro calls it internally when needed.
 * It blocks until it has joined the poller thread and, with
 * num_reactor_threads > 1, until every dispatch worker has finished its
 * current job and been joined too. It's a graceful drain, not a cancel:
 * after this call returns no dispatch can be in flight, the same guarantee
 * num_reactor_threads == 1 gives.
 *
 * From then on no queue registration of loop takes a wake from its queue,
 * and a wake it was holding goes to the queue's next listener, so the
 * queue's other listeners keep getting every message. Each registration
 * stays in place until ccol_event_loop_remove() or ccol_event_loop_destroy(),
 * which still runs its on_removed, and a later ccol_event_loop_add() on loop
 * is refused.
 *
 * @param loop ccol_event_loop to shut down
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop is CCOL_EVENT_LOOP_NULL, or if loop is
 * a stale handle or a handle that a call already destroyed
 * @return ccol_not_permitted if the call comes from inside a callback running
 * on one of this loop's threads: the poller or, for num_reactor_threads > 1,
 * a dispatch worker. See the warning below.
 *
 * @warning Do not call this from inside a callback running on one of this
 * loop's threads. That call would join the thread from itself, which is
 * undefined behaviour and gives EDEADLK, so the function detects the case and
 * refuses it with ccol_not_permitted rather than leave a caller a way to
 * trigger undefined behaviour. Move the shutdown to another thread, or do it
 * after the callback returns.
 */
ccol_retval_t ccol_event_loop_shutdown(ccol_event_loop loop);

/**
 * @brief Internal destroy. Use the ccol_event_loop_destroy() macro.
 *
 * Shuts the reactor thread down, unless a call already did, and then frees
 * every registration that is left.
 *
 * The shutdown is the same graceful drain as ccol_event_loop_shutdown(): the
 * callbacks the loop already collected run, and loop stays a live handle for
 * them until the drain ends. So a callback can call ccol_event_loop_remove(),
 * ccol_event_loop_pause(), ccol_event_loop_resume(),
 * ccol_event_loop_modify() and ccol_event_loop_add() on loop during the
 * destroy, with the same results as during ccol_event_loop_shutdown(). That
 * makes the documented way to close a connection from its own callback
 * (remove every registration of it, close the fd, free arg) safe while
 * another thread destroys the loop: once the removals return, no callback of
 * those registrations runs again. When the drain ends loop stops resolving,
 * and every later call on it fails the way it does for a destroyed handle.
 * For a registration
 * backed by a queue it first unlinks the registration from that queue's
 * waiter list, so a queue that outlives this ccol_event_loop is never left
 * with a waiter pointer to nowhere.
 *
 * The reverse order is the caller's job. Before destroying a queue that was
 * registered with ccol_selectable_from_circq, ccol_selectable_from_dynq, or
 * ccol_selectable_from_chan, the caller must remove the registration with
 * ccol_event_loop_remove(), or else every ccol_select() and
 * ccol_select_timed() call that watches the queue must have returned.
 * Destroying the queue before that is a caller bug, which
 * ccol_circular_queue_destroy(), ccol_dynamic_queue_destroy(), and
 * ccol_channel_destroy() detect and assert against. Without that assert the
 * waiter node would still be linked and pointing to the queue's mutex, which
 * the destroy is about to free.
 *
 * loop must be a live handle: one that ccol_event_loop_create or
 * ccol_event_loop_create_mp gave out and that hasn't been destroyed yet. A
 * stale handle is a fatal error. A handle is stale when an earlier, finished
 * call to this function destroyed it, or when another thread is destroying it
 * right now in a race with this call. A forged or garbage value is a fatal
 * error too.
 * In those cases the function calls _ccol_fatal_err(), which aborts with
 * SIGABRT, instead of risking a use-after-free or a double free. The rule
 * covers both a plain double destroy one after the other and a concurrent
 * one that overlaps in time. CCOL_EVENT_LOOP_NULL (0) is the one
 * exception and stays a silent no-op, which fits the ccol_event_loop_destroy
 * idiom of setting the handle to the invalid value.
 *
 * Calling this on loop from inside a callback that runs on one of loop's
 * threads (the poller thread or, for num_reactor_threads > 1, a dispatch
 * worker) is a fatal error for the same reason. The call would have to join
 * that thread through an internal ccol_event_loop_shutdown, and it would also
 * free every registration and the loop struct itself while a callback is
 * still running on that thread, which is a use-after-free and not only a
 * deadlock. Move the destroy to another thread, or do it after the callback
 * returns.
 *
 * @param loop ccol_event_loop to destroy
 *
 * @warning Do not call this function directly. Use the
 * ccol_event_loop_destroy() macro.
 */
void __ccol_event_loop_destroy(ccol_event_loop loop);

/**
 * @brief RAII cleanup function. Use it with _ccol_destructor.
 *
 * Calling this on an *lp that is already CCOL_EVENT_LOOP_NULL does
 * nothing. Calling it on a stale handle that isn't CCOL_EVENT_LOOP_NULL
 * and was already destroyed through another path is the same fatal misuse
 * that __ccol_event_loop_destroy documents.
 */
static inline __attribute__((always_inline)) void ___ccol_event_loop_destroy(
    ccol_event_loop *lp) {
  if (lp && *lp) {
    __ccol_event_loop_destroy(*lp);
    *lp = CCOL_EVENT_LOOP_NULL;
  }
  /* gcov can't count the closing brace of this always-inline function at
   * -O3, although its body runs, so the coverage report leaves that one line
   * out. Don't copy the marker to a line that a test can reach. */
} /* LCOV_EXCL_LINE */

/**
 * @brief Destroy a ccol_event_loop and set the handle to
 * CCOL_EVENT_LOOP_NULL
 *
 * Blocks until every in-flight resolved use of this handle has finished. Do
 * not call it concurrently with another call on the same handle; as the doc
 * comment of __ccol_event_loop_destroy explains, that is a fatal error, not a
 * silent race.
 *
 * @param loop ccol_event_loop to destroy. The macro sets it to
 * CCOL_EVENT_LOOP_NULL afterwards.
 *
 * @note You can call this with a CCOL_EVENT_LOOP_NULL handle
 * @warning Do not call this on loop from inside a callback that is being
 * dispatched on one of loop's threads. That's a self-destroy hazard, the
 * destroy-side twin of ccol_event_loop_shutdown's self-join hazard, and it is
 * a fatal error that aborts with SIGABRT, neither a silent no-op nor a
 * deadlock. Move the destroy to another thread, or do it after the
 * callback returns.
 *
 * @note The macro evaluates loop exactly once, and loop must be a modifiable
 * lvalue such as a variable or an array element
 */
#define ccol_event_loop_destroy(loop) \
  _ccol_event_loop_destroy_impl(      \
      loop, _ccol_uniq(__ccol_event_loop_destroy_slot, __COUNTER__))

/* Internal body of ccol_event_loop_destroy. slot is a name from
 * _ccol_uniq(), so the macro can nest inside the argument of another destroy
 * macro and stay -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_event_loop_destroy_impl(loop, slot) \
  do {                                            \
    __typeof__(loop) *slot = &(loop);             \
    __ccol_event_loop_destroy(*slot);             \
    *slot = CCOL_EVENT_LOOP_NULL;                 \
  } while (0)

/**
 * @brief Declare a ccol_event_loop variable that has no value yet
 *
 * Follow it with a ccol_event_loop_construct or ccol_event_loop_create call.
 */
#define ccol_event_loop_declare(name) ccol_event_loop name

/**
 * @brief Declare a variable that is destroyed at the end of its scope
 */
#define ccol_event_loop_declare_scoped(name)                          \
  ccol_event_loop name _ccol_destructor(___ccol_event_loop_destroy) = \
      CCOL_EVENT_LOOP_NULL

/**
 * @brief Declare and initialize in one step. On failure, call _ccol_fatal_err.
 *
 * Example:
 * @code
 * ccol_event_loop_construct(loop, 32, 1, 1);
 * ccol_selectable sel = ccol_selectable_from_fd(fd, ccol_select_read);
 * ccol_event_reg r = ccol_event_loop_add(loop, sel, handlers, NULL, NULL);
 * // ... Remove r with ccol_event_loop_remove() before you destroy loop ...
 * ccol_event_loop_destroy(loop);
 * @endcode
 *
 * @param name               Name of the variable for the ccol_event_loop
 *                            handle
 * @param max_events_per_wait Size of the epoll_wait batch buffer. The value
 *                            must be from 1 to INT_MAX, and small enough
 *                            that max_events_per_wait *
 *                            sizeof(struct epoll_event) does not overflow
 *                            size_t.
 * @param num_lock_stripes   Number of lock stripes for the registry of fds
 *                           and entries. The value must be >= 1, and a value
 *                           of 1 means one registry lock for the whole loop.
 * @param num_reactor_threads Number of background reactor threads. The value
 *                           must be >= 1, and a value of 1 means one thread
 *                           that polls and dispatches in line.
 */
#define ccol_event_loop_construct(name, max_events_per_wait, num_lock_stripes, \
                                  num_reactor_threads)                         \
  ccol_event_loop name = CCOL_EVENT_LOOP_NULL;                                 \
  do {                                                                         \
    char *_evl_err = NULL;                                                     \
    (name) = ccol_event_loop_create((max_events_per_wait), (num_lock_stripes), \
                                    (num_reactor_threads), &_evl_err);         \
    if (!(name)) {                                                             \
      _ccol_fatal_err("ccol_event_loop_construct('%s'): %s", #name,            \
                      _evl_err ? _evl_err : "unknown error");                  \
    }                                                                          \
  } while (0)

/**
 * @brief Declare, initialize, and destroy at the end of the scope. On
 *        failure, call _ccol_fatal_err.
 *
 * @param name               Name of the variable for the ccol_event_loop
 *                            handle
 * @param max_events_per_wait Size of the epoll_wait batch buffer. The value
 *                            must be from 1 to INT_MAX, and small enough
 *                            that max_events_per_wait *
 *                            sizeof(struct epoll_event) does not overflow
 *                            size_t.
 * @param num_lock_stripes   Number of lock stripes for the registry of fds
 *                           and entries. The value must be >= 1, and a value
 *                           of 1 means one registry lock for the whole loop.
 * @param num_reactor_threads Number of background reactor threads. The value
 *                           must be >= 1, and a value of 1 means one thread
 *                           that polls and dispatches in line.
 */
#define ccol_event_loop_construct_scoped(                                      \
    name, max_events_per_wait, num_lock_stripes, num_reactor_threads)          \
  ccol_event_loop name _ccol_destructor(___ccol_event_loop_destroy) =          \
      CCOL_EVENT_LOOP_NULL;                                                    \
  do {                                                                         \
    char *_evl_err = NULL;                                                     \
    (name) = ccol_event_loop_create((max_events_per_wait), (num_lock_stripes), \
                                    (num_reactor_threads), &_evl_err);         \
    if (!(name)) {                                                             \
      _ccol_fatal_err("ccol_event_loop_construct_scoped('%s'): %s", #name,     \
                      _evl_err ? _evl_err : "unknown error");                  \
    }                                                                          \
  } while (0)

#pragma GCC visibility pop
