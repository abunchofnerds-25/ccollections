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

/* Everything that this header declares from here to the end of the file is
 * part of the public Application Binary Interface (ABI) of libccollections.
 * The shared library exports all of it. The library itself is built with
 * -fvisibility=hidden. Any function or object that is not inside one of these
 * blocks therefore stays internal to the library. It is absent from the
 * dynamic symbol table. The application that links against the library cannot
 * interpose it. A symbol of the same name in that application cannot collide
 * with it. */
#pragma GCC visibility push(default)

/**
 * @file cthreadcomm.h
 * @brief Thread-safe message passing primitives for communication between
 * threads
 *
 * This header gives three thread-safe ways to pass messages:
 * - ccol_circular_queue: a fixed-size queue. A send blocks while the queue is
 *   full. This is how the queue applies backpressure.
 * - ccol_dynamic_queue: a queue with no size limit (a linked list). A send
 *   never blocks.
 * - ccol_channel: two-way communication between an owner thread and a worker
 *   thread
 *
 * All operations are zero-copy. They transfer the ownership of a pointer,
 * which is efficient.
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
 * A message transfers the ownership of the data pointer. After a send that
 * succeeds, the library sets the data pointer of the sender to NULL. The
 * receiver gets the ownership. The receiver must free the memory.
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
 * This function creates a queue with a fixed size. Inside, the queue uses a
 * circular buffer. A send blocks while the queue is full. It waits for free
 * space.
 *
 * @param max_size Maximum number of messages that the queue can hold. The
 * value must be from 1 to SIZE_MAX / sizeof(c_message_t). A larger value
 * gives ccol_invalid_args. One array of max_size * sizeof(c_message_t) bytes
 * holds the queue, and a larger value makes that computation overflow.
 * @param mmgmt_procs Custom memory management procedures, or NULL to use the
 * default malloc and free
 * @param err_str Optional pointer that gets an error string on failure. Pass
 * NULL to ignore it.
 *
 * @return Pointer to the new circular queue, or NULL on failure
 *
 * @note All operations on this queue are thread-safe
 * @note You must destroy the queue with ccol_circular_queue_destroy() after
 * you finish with it
 *
 * @see ccol_circular_queue_create
 * @see ccol_circular_queue_destroy
 */
ccol_circular_queue *ccol_circular_queue_create_with_mprocs(
    size_t max_size, ccol_memmgmt_procs_t *mmgmt_procs, char **err_str);

/**
 * @brief Create a circular queue with default memory management
 *
 * This macro creates a circular queue with the standard malloc and free.
 *
 * @param max_size Maximum number of messages that the queue can hold
 * @param err_str Optional pointer that gets an error string on failure
 *
 * @return Pointer to the new circular queue, or NULL on failure
 */
#define ccol_circular_queue_create(max_size, err_str) \
  ccol_circular_queue_create_with_mprocs((max_size), NULL, (err_str))

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
 * This macro frees all the resources of the queue. The macro calls
 * __ccol_circular_queue_destroy(). That function asserts in two cases. The
 * first case is a queue that still holds messages. The second case is a queue
 * that a watcher still watches. A watcher is a ccol_select() call, a
 * ccol_select_timed() call, or an ccol_event_loop registration
 * (ccol_event_loop_add with ccol_selectable_from_circq or
 * ccol_selectable_from_chan). Drain the queue before you destroy it. Then call
 * ccol_event_loop_remove(). You can also let every ccol_select() call and
 * every ccol_select_timed() call that watches this queue return.
 *
 * That sequence is safe as written. ccol_event_loop_remove() returns without
 * a wait for a callback that it already gave to a reactor thread. Such a
 * callback can therefore still be inside this queue when the removal returns.
 * This destroy blocks until every one of those callbacks finishes with the
 * queue. Then it frees the queue.
 *
 * The destroy counts the messages that stay in the queue only after those
 * callbacks finished. A write callback that was already running when
 * ccol_event_loop_remove() returned can still send, and its message then
 * makes the destroy assert. When a write registration watches the queue,
 * remove it first, wait for its on_removed callback, and only then drain
 * and destroy the queue.
 *
 * A callback that the loop runs for a different registration can run this
 * whole sequence for this queue, with any num_reactor_threads. A dispatch
 * that the loop collected for this queue and did not start yet does not hold
 * the destroy up, and it never runs its callback after the removal.
 *
 * @param cq Circular queue to destroy. The macro sets it to NULL after the
 * destroy.
 *
 * @warning The macro does not free the messages that stay in the queue
 * @warning Do not destroy a queue that a ccol_select() call, a
 * ccol_select_timed() call, or an ccol_event_loop registration still watches.
 * The waiter node of the watcher still points to the mutex of this queue, so
 * this is a use-after-free. The function asserts against it, in the same way
 * as it asserts against a queue that is not empty.
 * @warning Do not call this from inside an ccol_event_loop callback that
 * dispatches this same queue. This is a caller error, and the program stops
 * through ccol_fatal_err(). The destroy would wait for that same callback.
 * Destroy the queue from a thread that does not run one of its callbacks.
 * @warning Do not hold an application lock across this call if one of the
 * callbacks of this queue takes that lock. The destroy waits for that
 * callback, and the callback would wait for the lock.
 * @note You can call this with a NULL pointer
 * @note The macro evaluates cq exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define ccol_circular_queue_destroy(cq) \
  _ccol_circular_queue_destroy_impl(    \
      cq, _ccol_uniq(__ccol_cq_destroy_slot, __COUNTER__))

/* Internal. The body of ccol_circular_queue_destroy. slot is a name from
 * _ccol_uniq(), so the macro nests inside the argument of another destroy
 * macro and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_circular_queue_destroy_impl(cq, slot) \
  do {                                              \
    __typeof__(cq) *slot = &(cq);                   \
    __ccol_circular_queue_destroy(*slot);           \
    *slot = NULL;                                   \
  } while (0)

/**
 * @brief Send a message to the queue (it blocks, zero-copy)
 *
 * This function blocks until the queue has free space. Then it transfers the
 * ownership of the message data to the queue. On success it sets the msg->data
 * pointer to NULL.
 *
 * @param cq Circular queue to send to
 * @param msg Message to send. On success the ownership of the data moves to
 * the queue.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or msg is NULL, or if the check of msg fails
 * @return ccol_not_permitted if sends on the queue are turned off
 *
 * @note The function blocks with no time limit. It waits for free space, or
 * for sends on the queue to go off.
 * @note After the function wakes up, it checks again whether sends are still
 * on
 * @note On failure, the caller keeps the ownership of msg->data
 * @note msg->data == NULL with msg->size == 0 is valid (a sentinel message)
 * @note msg->data == NULL with msg->size > 0 is not valid
 *
 * @see ccol_circq_try_send_zc
 * @see ccol_circq_timed_send_zc
 */
ccol_retval_t ccol_circq_send_zc(ccol_circular_queue *cq, c_message_t *msg);

/**
 * @brief Try to send a message with no block (zero-copy)
 *
 * This function tries to send the message immediately. If the queue is full,
 * it returns ccol_container_full immediately. On success it transfers the
 * ownership of msg->data.
 *
 * @param cq Circular queue to send to
 * @param msg Message to send. On success the ownership of the data moves to
 * the queue.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or msg is NULL, or if the check of msg fails
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
 * This function blocks and waits for free space. It waits for the given
 * timeout at most. If free space arrives, the function sends the message and
 * transfers the ownership. If the timeout ends, or if sends go off, the
 * function returns the correct error code.
 *
 * @param cq Circular queue to send to
 * @param msg Message to send. On success the ownership of the data moves to
 * the queue.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 does not wait: the call then does exactly what
 * ccol_circq_try_send_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline
 * that the clock can hold and in practice waits with no end; it never wraps
 * into the past.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or msg is NULL, or if the check of msg fails
 * @return ccol_not_permitted if sends on the queue are turned off
 * @return ccol_container_full if timeout_us is 0 and the queue is full
 * @return ccol_timed_out if the timeout ends before free space arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for the duration of the timeout at most
 * @note After the function wakes up, it checks again whether sends are still
 * on
 * @note On failure, the caller keeps the ownership of msg->data
 * @note The library measures the timeout against CLOCK_MONOTONIC. A change to
 * the wall clock of the system does not make the timeout longer or shorter.
 * An administrator, NTP, or the resume of a virtual machine can change that
 * clock. The call waits for the duration that the caller gave.
 *
 * @see ccol_circq_send_zc
 * @see ccol_circq_try_send_zc
 */
ccol_retval_t ccol_circq_timed_send_zc(ccol_circular_queue *cq,
                                       c_message_t *msg, uint64_t timeout_us);

/**
 * @brief Receive a message from the queue (it blocks, zero-copy)
 *
 * This function blocks until a message arrives. Then it transfers the
 * ownership of the message data to the caller in target_buf.
 *
 * @param cq Circular queue to receive from
 * @param target_buf Buffer that gets the message. It also gets the ownership
 * of the data.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq or target_buf is NULL
 *
 * @note The function blocks with no time limit. It waits for a message.
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
 * This function tries to receive a message immediately. If the queue is empty,
 * it returns ccol_container_empty immediately. On success it transfers the
 * ownership of the message data.
 *
 * @param cq Circular queue to receive from
 * @param target_buf Buffer that gets the message. It also gets the ownership
 * of the data.
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
 * This function blocks and waits for a message. It waits for the given timeout
 * at most. If a message arrives, the function receives it and transfers the
 * ownership. If the timeout ends, the function returns ccol_timed_out.
 *
 * @param cq Circular queue to receive from
 * @param target_buf Buffer that gets the message. It also gets the ownership
 * of the data.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 does not wait: the call then does exactly what
 * ccol_circq_try_recv_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline
 * that the clock can hold and in practice waits with no end; it never wraps
 * into the past.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if cq or target_buf is NULL
 * @return ccol_container_empty if timeout_us is 0 and the queue is empty
 * @return ccol_timed_out if the timeout ends before a message arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for the duration of the timeout at most
 * @note On success, the caller gets the ownership of target_buf->data
 * @note The library measures the timeout against CLOCK_MONOTONIC. A change to
 * the wall clock of the system does not make the timeout longer or shorter.
 * An administrator, NTP, or the resume of a virtual machine can change that
 * clock. The call waits for the duration that the caller gave.
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
 * This function stops all new sends. It also wakes every thread that blocks
 * inside a send. Each of those senders returns ccol_not_permitted. A receive
 * does not change.
 *
 * @param cq Circular queue that gets sends turned off
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq is NULL
 *
 * @note The function wakes every blocked sender immediately with a broadcast
 * @note ccol_circq_enable_sending() turns sends on again
 * @note The function does not change a receive
 *
 * @see ccol_circq_enable_sending
 */
ccol_retval_t ccol_circq_disable_sending(ccol_circular_queue *cq);

/**
 * @brief Turn on sends on the queue
 *
 * This function turns sends on again after a call turned them off. It also
 * wakes every thread that blocks and waits to send. Those senders can then
 * continue.
 *
 * @param cq Circular queue that gets sends turned on
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if cq is NULL
 *
 * @note The function wakes every blocked sender with a broadcast
 * @note A thread can try to send immediately if the queue has free space
 *
 * @see ccol_circq_disable_sending
 */
ccol_retval_t ccol_circq_enable_sending(ccol_circular_queue *cq);

/**
 * @brief Get the current message count of the queue
 *
 * This function gives the number of messages that the queue holds now.
 *
 * @param cq Circular queue to ask
 *
 * @return Number of messages in the queue, or (size_t)-1 on an error
 *
 * @note The count is a thread-safe snapshot
 * @note A return value of (size_t)-1 shows an error (a NULL queue)
 */
size_t ccol_circq_msg_count(ccol_circular_queue *cq);

/* ========================================================================== */
/*                          DYNAMIC QUEUE FUNCTIONS                           */
/* ========================================================================== */

/**
 * @brief Create a dynamic queue with custom memory management
 *
 * This function creates a queue with no size limit. The queue uses a
 * doubly-linked list. A send never blocks, but it can fail if the allocation
 * of memory fails. The queue can grow to ccol_max_elem_count messages.
 *
 * @param mmgmt_procs Custom memory management procedures, or NULL to use the
 * default malloc and free
 * @param err_str Optional pointer that gets an error string on failure. Pass
 * NULL to ignore it.
 *
 * @return Pointer to the new dynamic queue, or NULL on failure
 *
 * @note A send never blocks. Only the free memory limits it.
 * @note The maximum size is ccol_max_elem_count, which common.h defines
 * @note You must destroy the queue with ccol_dynamic_queue_destroy() after you
 * finish with it
 *
 * @see ccol_dynamic_queue_create
 * @see ccol_dynamic_queue_destroy
 */
ccol_dynamic_queue *ccol_dynamic_queue_create_with_mprocs(
    ccol_memmgmt_procs_t *mmgmt_procs, char **err_str);

/**
 * @brief Create a dynamic queue with default memory management
 *
 * This macro creates a dynamic queue with the standard malloc and free.
 *
 * @param err_str Optional pointer that gets an error string on failure
 *
 * @return Pointer to the new dynamic queue, or NULL on failure
 */
#define ccol_dynamic_queue_create(err_str) \
  ccol_dynamic_queue_create_with_mprocs(NULL, (err_str))

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
 * This macro frees all the resources of the queue. This includes every node of
 * the linked list. The macro calls __ccol_dynamic_queue_destroy(). That
 * function asserts in two cases. The first case is a queue that still holds
 * messages. The second case is a queue that a watcher still watches. A watcher
 * is a ccol_select() call, a ccol_select_timed() call, or an ccol_event_loop
 * registration (ccol_event_loop_add with ccol_selectable_from_dynq). Drain the
 * queue before you destroy it. Then call ccol_event_loop_remove(). You can
 * also let every ccol_select() call and every ccol_select_timed() call that
 * watches this queue return.
 *
 * That sequence is safe as written. ccol_event_loop_remove() returns without
 * a wait for a callback that it already gave to a reactor thread. Such a
 * callback can therefore still be inside this queue when the removal returns.
 * This destroy blocks until every one of those callbacks finishes with the
 * queue. Then it frees the queue.
 *
 * The destroy counts the messages that stay in the queue only after those
 * callbacks finished. A write callback that was already running when
 * ccol_event_loop_remove() returned can still send, and its message then
 * makes the destroy assert. When a write registration watches the queue,
 * remove it first, wait for its on_removed callback, and only then drain
 * and destroy the queue.
 *
 * A callback that the loop runs for a different registration can run this
 * whole sequence for this queue, with any num_reactor_threads. A dispatch
 * that the loop collected for this queue and did not start yet does not hold
 * the destroy up, and it never runs its callback after the removal.
 *
 * @param dq Dynamic queue to destroy. The macro sets it to NULL after the
 * destroy.
 *
 * @warning The macro does not free the messages that stay in the queue
 * @warning Do not destroy a queue that a ccol_select() call, a
 * ccol_select_timed() call, or an ccol_event_loop registration still watches.
 * The waiter node of the watcher still points to the mutex of this queue, so
 * this is a use-after-free. The function asserts against it, in the same way
 * as it asserts against a queue that is not empty.
 * @warning Do not call this from inside an ccol_event_loop callback that
 * dispatches this same queue. This is a caller error, and the program stops
 * through ccol_fatal_err(). The destroy would wait for that same callback.
 * Destroy the queue from a thread that does not run one of its callbacks.
 * @warning Do not hold an application lock across this call if one of the
 * callbacks of this queue takes that lock. The destroy waits for that
 * callback, and the callback would wait for the lock.
 * @note You can call this with a NULL pointer
 * @note The macro evaluates dq exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define ccol_dynamic_queue_destroy(dq) \
  _ccol_dynamic_queue_destroy_impl(    \
      dq, _ccol_uniq(__ccol_dq_destroy_slot, __COUNTER__))

/* Internal. The body of ccol_dynamic_queue_destroy. slot is a name from
 * _ccol_uniq(), so the macro nests inside the argument of another destroy
 * macro and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_dynamic_queue_destroy_impl(dq, slot) \
  do {                                             \
    __typeof__(dq) *slot = &(dq);                  \
    __ccol_dynamic_queue_destroy(*slot);           \
    *slot = NULL;                                  \
  } while (0)

/**
 * @brief Send a message to the dynamic queue (no block, zero-copy)
 *
 * This function sends a message with no block. It allocates a new list node.
 * It puts the message at the tail of the queue. It transfers the ownership of
 * msg->data.
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
 * @note The function never blocks. It returns immediately with a success code
 * or an error code.
 * @note On failure, the caller keeps the ownership of msg->data
 * @note There is no send that blocks and no send with a timeout. The queue has
 * no size limit by design.
 * @note Only ccol_max_elem_count and the free memory limit the queue
 * @note msg->data == NULL with msg->size == 0 is valid (a sentinel message)
 *
 * @see ccol_dynmq_recv_zc
 */
ccol_retval_t ccol_dynmq_send_zc(ccol_dynamic_queue *dq, c_message_t *msg);

/**
 * @brief Receive a message from the dynamic queue (it blocks, zero-copy)
 *
 * This function blocks until a message arrives. Then it removes the message
 * from the head of the queue. It transfers the ownership to the caller.
 *
 * @param dq Dynamic queue to receive from
 * @param target_buf Buffer that gets the message. It also gets the ownership
 * of the data.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if dq or target_buf is NULL
 *
 * @note The function blocks with no time limit. It waits for a message.
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
 * This function tries to receive a message immediately. If the queue is empty,
 * it returns ccol_container_empty immediately.
 *
 * @param dq Dynamic queue to receive from
 * @param target_buf Buffer that gets the message. It also gets the ownership
 * of the data.
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
 * This function blocks and waits for a message. It waits for the given timeout
 * at most. If a message arrives, the function receives it and transfers the
 * ownership.
 *
 * @param dq Dynamic queue to receive from
 * @param target_buf Buffer that gets the message. It also gets the ownership
 * of the data.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 does not wait: the call then does exactly what
 * ccol_dynmq_try_recv_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline
 * that the clock can hold and in practice waits with no end; it never wraps
 * into the past.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if dq or target_buf is NULL
 * @return ccol_container_empty if timeout_us is 0 and the queue is empty
 * @return ccol_timed_out if the timeout ends before a message arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for the duration of the timeout at most
 * @note On success, the caller gets the ownership of target_buf->data
 * @note The library measures the timeout against CLOCK_MONOTONIC. A change to
 * the wall clock of the system does not make the timeout longer or shorter.
 * An administrator, NTP, or the resume of a virtual machine can change that
 * clock. The call waits for the duration that the caller gave.
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
 * This function stops all new sends. A dynamic queue has no blocked senders,
 * because a send never blocks. A circular queue is different. This call
 * therefore changes only the sends that come later.
 *
 * @param dq Dynamic queue that gets sends turned off
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if dq is NULL
 *
 * @note ccol_dynmq_enable_sending() turns sends on again
 * @note The function does not change a receive
 *
 * @see ccol_dynmq_enable_sending
 */
ccol_retval_t ccol_dynmq_disable_sending(ccol_dynamic_queue *dq);

/**
 * @brief Turn on sends on the dynamic queue
 *
 * This function turns sends on again after a call turned them off.
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
 * This function gives the number of messages that the queue holds now.
 *
 * @param dq Dynamic queue to ask
 *
 * @return Number of messages in the queue, or (size_t)-1 on an error
 *
 * @note The count is a thread-safe snapshot
 * @note A return value of (size_t)-1 shows an error (a NULL queue)
 */
size_t ccol_dynmq_msg_count(ccol_dynamic_queue *dq);

/* ========================================================================== */
/*                            CHANNEL FUNCTIONS                               */
/* ========================================================================== */

/**
 * @brief Create a two-way ccol_channel with custom memory management
 *
 * This function creates a ccol_channel. Inside, the channel holds two circular
 * queues. They give two-way communication between an owner thread and the
 * worker threads. The thread that calls this function becomes the owner. The
 * channel picks the direction itself from the ID of the thread that calls it.
 *
 * @param max_size Maximum number of messages that each direction can hold. One
 * circular queue holds each direction, so this value has the same bound as the
 * max_size of ccol_circular_queue_create_with_mprocs. The bound is
 * SIZE_MAX / sizeof(c_message_t).
 * @param mmgmt_procs Custom memory management procedures, or NULL to use the
 * default malloc and free
 * @param err_str Optional pointer that gets an error string on failure. Pass
 * NULL to ignore it.
 *
 * @return Pointer to the new ccol_channel, or NULL on failure
 *
 * @note The channel holds two circular queues: ccol_owner_to_workers and
 * ccol_workers_to_owner
 * @note The thread that creates the channel is the owner. Every other thread
 * is a worker.
 * @note The channel picks the direction itself from the ID of the thread
 * @note You must destroy the ccol_channel with ccol_channel_destroy() after
 * you finish with it
 * @note The owner thread must stay alive for all the time that the
 * ccol_channel is in use. To route a message, the channel compares the ID of
 * the thread that calls it against the ID that it took at creation time. After
 * the first owner exits, the OS can give the same thread-ID value to a new,
 * unrelated thread. The channel then routes that thread as the owner too, and
 * it reports nothing.
 *
 * @see ccol_channel_create
 * @see ccol_channel_destroy
 */
ccol_channel *ccol_channel_create_with_mprocs(size_t max_size,
                                              ccol_memmgmt_procs_t *mmgmt_procs,
                                              char **err_str);

/**
 * @brief Create a two-way ccol_channel with default memory management
 *
 * This macro creates a ccol_channel with the standard malloc and free.
 *
 * @param max_size Maximum number of messages that each direction can hold
 * @param err_str Optional pointer that gets an error string on failure
 *
 * @return Pointer to the new ccol_channel, or NULL on failure
 */
#define ccol_channel_create(max_size, err_str) \
  ccol_channel_create_with_mprocs((max_size), NULL, (err_str))

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
 * This macro frees all the resources of the ccol_channel. This includes both
 * of the circular queues inside it. The macro calls __ccol_channel_destroy().
 * That function asserts in two cases. The first case is a queue below the
 * channel that still holds messages. The second case is a direction that a
 * watcher still watches. A watcher is a ccol_select() call, a
 * ccol_select_timed() call, or an ccol_event_loop registration
 * (ccol_event_loop_add with ccol_selectable_from_chan). Drain both directions
 * before you destroy the channel. Then call ccol_event_loop_remove(). You can
 * also let every ccol_select() call and every ccol_select_timed() call that
 * watches one of the directions return.
 *
 * That sequence is safe as written. ccol_event_loop_remove() returns without
 * a wait for a callback that it already gave to a reactor thread. Such a
 * callback can therefore still be inside one of the queues below the channel
 * when the removal returns. This destroy blocks until every one of those
 * callbacks finishes with that queue. Then it frees the queue.
 *
 * A callback that the loop runs for a different registration can run this
 * whole sequence for this channel, with any num_reactor_threads. A dispatch
 * that the loop collected for the channel and did not start yet does not hold
 * the destroy up, and it never runs its callback after the removal.
 *
 * @param ch Channel to destroy. The macro sets it to NULL after the destroy.
 *
 * @warning The macro does not free the messages that stay in one of the
 * directions
 * @warning Do not destroy a ccol_channel that a ccol_select() call, a
 * ccol_select_timed() call, or an ccol_event_loop registration still watches.
 * The waiter node of the watcher still points to the mutex of the queue below
 * the channel, so this is a use-after-free. The function asserts against it,
 * in the same way as it asserts against messages that stay in one of the
 * directions.
 * @warning Do not call this from inside an ccol_event_loop callback that
 * dispatches one of the directions of this ccol_channel. This is a caller
 * error, and the program stops through ccol_fatal_err(). The destroy would
 * wait for that same callback.
 * @warning Do not hold an application lock across this call if one of the
 * callbacks of this ccol_channel takes that lock. The destroy waits for that
 * callback, and the callback would wait for the lock.
 * @note You can call this with a NULL pointer
 * @note The macro evaluates ch exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define ccol_channel_destroy(ch) \
  _ccol_channel_destroy_impl(    \
      ch, _ccol_uniq(__ccol_channel_destroy_slot, __COUNTER__))

/* Internal. The body of ccol_channel_destroy. slot is a name from
 * _ccol_uniq(), so the macro nests inside the argument of another destroy
 * macro and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_channel_destroy_impl(ch, slot) \
  do {                                       \
    __typeof__(ch) *slot = &(ch);            \
    __ccol_channel_destroy(*slot);           \
    *slot = NULL;                            \
  } while (0)

/**
 * @brief Send a message through the ccol_channel (it blocks, zero-copy)
 *
 * The function picks the correct queue itself. It uses the thread that calls
 * it:
 * - The owner thread sends to the ccol_owner_to_workers queue
 * - A worker thread sends to the ccol_workers_to_owner queue
 *
 * @param ch Channel to send through
 * @param msg Message to send. On success the ownership of the data moves to
 * the channel.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch or msg is NULL, or if the check of msg fails
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
 * This function tries to send the message immediately. It uses the queue that
 * the channel picks itself. If that queue is full, the function returns
 * immediately.
 *
 * @param ch Channel to send through
 * @param msg Message to send. On success the ownership of the data moves to
 * the channel.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch or msg is NULL, or if the check of msg fails
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
 * This function blocks and waits for free space in the queue that the channel
 * picks itself. It waits for the given timeout at most.
 *
 * @param ch Channel to send through
 * @param msg Message to send. On success the ownership of the data moves to
 * the channel.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 does not wait: the call then does exactly what
 * ccol_chan_try_send_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline
 * that the clock can hold and in practice waits with no end; it never wraps
 * into the past.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch or msg is NULL, or if the check of msg fails
 * @return ccol_not_permitted if sends on the chosen direction are turned off
 * @return ccol_container_full if timeout_us is 0 and the chosen queue is full
 * @return ccol_timed_out if the timeout ends before free space arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for the duration of the timeout at most
 * @note The function picks the direction itself from the ID of the thread
 * @note The library measures the timeout against CLOCK_MONOTONIC. A change to
 * the wall clock of the system does not make the timeout longer or shorter.
 * An administrator, NTP, or the resume of a virtual machine can change that
 * clock. The call waits for the duration that the caller gave.
 *
 * @see ccol_chan_send_zc
 * @see ccol_chan_try_send_zc
 */
ccol_retval_t ccol_chan_timed_send_zc(ccol_channel *ch, c_message_t *msg,
                                      uint64_t timeout_us);

/**
 * @brief Receive a message from the ccol_channel (it blocks, zero-copy)
 *
 * The function picks the correct queue itself. It uses the thread that calls
 * it:
 * - The owner thread receives from the ccol_workers_to_owner queue
 * - A worker thread receives from the ccol_owner_to_workers queue
 *
 * @param ch Channel to receive from
 * @param target_buf Buffer that gets the message. It also gets the ownership
 * of the data.
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
 * This function tries to receive a message immediately. It uses the queue that
 * the channel picks itself. If that queue is empty, the function returns
 * immediately.
 *
 * @param ch Channel to receive from
 * @param target_buf Buffer that gets the message. It also gets the ownership
 * of the data.
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
 * This function blocks and waits for a message in the queue that the channel
 * picks itself. It waits for the given timeout at most.
 *
 * @param ch Channel to receive from
 * @param target_buf Buffer that gets the message. It also gets the ownership
 * of the data.
 * @param timeout_us The longest time to wait, in microseconds, measured from
 * the call. 0 does not wait: the call then does exactly what
 * ccol_chan_try_recv_zc() does and returns its result. A value too large to
 * form a deadline, such as UINT64_MAX, saturates to the latest deadline
 * that the clock can hold and in practice waits with no end; it never wraps
 * into the past.
 *
 * @return ccol_success on success. The caller must free target_buf->data.
 * @return ccol_invalid_args if ch or target_buf is NULL
 * @return ccol_container_empty if timeout_us is 0 and the chosen queue is empty
 * @return ccol_timed_out if the timeout ends before a message arrives
 * @return ccol_unexpected_failure if the wait fails with an error other than a
 * timeout; errno then holds that error
 *
 * @note The function blocks for the duration of the timeout at most
 * @note The function picks the direction itself from the ID of the thread
 * @note The library measures the timeout against CLOCK_MONOTONIC. A change to
 * the wall clock of the system does not make the timeout longer or shorter.
 * An administrator, NTP, or the resume of a virtual machine can change that
 * clock. The call waits for the duration that the caller gave.
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
 * This function stops all new sends on the given direction. It also wakes
 * every thread that blocks inside a send on that direction.
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
 * @note The function does not change the other direction
 *
 * @see ccol_chan_enable_sending
 */
ccol_retval_t ccol_chan_disable_sending(ccol_channel *ch,
                                        ccol_channel_direction d);

/**
 * @brief Turn on sends on one direction of a ccol_channel
 *
 * This function turns sends on the given direction on again after a call
 * turned them off. It also wakes every thread that blocks and waits to send.
 *
 * @param ch Channel to change
 * @param d Direction that gets sends turned on (ccol_owner_to_workers or
 * ccol_workers_to_owner)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if ch is NULL or d is not valid
 *
 * @note The function wakes every blocked sender with a broadcast
 * @note A thread can try to send immediately if the queue has free space
 * @note The function does not change the other direction
 *
 * @see ccol_chan_disable_sending
 */
ccol_retval_t ccol_chan_enable_sending(ccol_channel *ch,
                                       ccol_channel_direction d);

/**
 * @brief Get the current message count of one ccol_channel direction
 *
 * This function gives the number of messages that the queue of the given
 * direction holds now.
 *
 * @param ch Channel to ask
 * @param d Direction to ask about (ccol_owner_to_workers or
 * ccol_workers_to_owner)
 *
 * @return Number of messages in the given direction, or (size_t)-1 on an error
 *
 * @note The count is a thread-safe snapshot
 * @note A return value of (size_t)-1 shows an error. The ccol_channel is NULL,
 * or the direction is not valid.
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
 * ccol_select_read waits until the queue holds one message or more to receive.
 * ccol_select_write waits until the queue has room for one send or more. For a
 * ccol_circular_queue the test is msg_count < max_size && !writing_disabled.
 * For a ccol_dynamic_queue the test is !writing_disabled && msg_count <
 * ccol_max_elem_count. That is the same ccol_container_full ceiling that
 * ccol_dynmq_send_zc() itself applies.
 *
 * ccol_select() does not consume or reserve either direction. After it wakes,
 * the caller must make the call itself. For a win on the read direction, call
 * ccol_circq_try_recv_zc or ccol_dynmq_try_recv_zc. For a win on the write
 * direction, call ccol_circq_try_send_zc or ccol_dynmq_send_zc. A TOCTOU race
 * is possible, as with POSIX select(2). This is why that call must not block.
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
 * Pass ccol_select_read to wait for a message to arrive. Pass
 * ccol_select_write to wait until the queue has room. For a channel, the macro
 * finds the correct queue below it when it builds the selectable. It uses the
 * ID of the thread that calls it, in the same way as ccol_chan_recv_zc() and
 * ccol_chan_send_zc().
 *
 * ccol_select() never does the receive or the send itself, for any type of
 * selectable. It only reports readiness. See the documentation of
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
 *  Inside, ccol_select uses epoll(7) when the array holds one fd selectable or
 *  more. It bridges the queue selectables in the same array with one
 *  eventfd(2) for each waiter. One epoll_wait call therefore reports both the
 *  readiness of an fd and the readiness of a queue.
 *
 *  ccol_select() never reads the fd and never writes to it. It only reports
 *  readiness. See the documentation of ccol_select(). The caller then does its
 *  own read(2), recv(2), write(2), or send(2) on fd_.
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
 *        the caller asks for. The function resolves the queue for the
 *        thread that calls it.
 *
 * The direction picks the queue. It also picks the waiter list that
 * ccol_select() uses inside:
 *
 *   ccol_select_read: the owner reads from workers_to_owner_cq, and a worker
 *                     reads from owner_to_workers_cq. This is the same as
 *                     ccol_chan_recv_zc.
 *   ccol_select_write: the owner writes to owner_to_workers_cq, and a worker
 *                     writes to workers_to_owner_cq. This is the same as
 *                     ccol_chan_send_zc.
 *
 * The function resolves the queue once, from the identity of the thread that
 * calls it. The ccol_selectable then watches that one queue, whichever thread
 * later waits on it or runs a callback for it.
 *
 * The readiness of the resolved queue works as it does for every other queue
 * selectable. The caller must do the receive or the send itself. ccol_select()
 * and ccol_event_loop only report that the queue is ready.
 *
 * After ccol_select() returns on the thread that built the selectable,
 * ccol_chan_try_recv_zc() and ccol_chan_try_send_zc() resolve to the same
 * queue, and so do ccol_circq_try_recv_zc() and ccol_circq_try_send_zc() on
 * sel.cq.
 *
 * Inside an ccol_event_loop callback, use ccol_circq_try_recv_zc() and
 * ccol_circq_try_send_zc() on the cq of the ccol_selectable that the callback
 * gets, and never a ccol_chan_* call. The callback runs on a thread of the
 * loop, and the channel counts that thread as a worker, whichever thread built
 * the selectable. A ccol_chan_* call there resolves for a worker, and for a
 * selectable that the owner built that is the other queue. Take a selectable
 * that the owner built for ccol_select_read: it watches workers_to_owner_cq.
 * A ccol_chan_try_recv_zc() inside its on_readable callback receives from
 * owner_to_workers_cq instead.
 * It takes a message that the owner sent to its workers, and it leaves the
 * message that made the callback run in the queue.
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
 * This function blocks until one selectable or more is ready for its own
 * direction. Then it sets *ready_index and returns. ccol_select() never does
 * the receive or the send itself, for a queue selectable or an fd selectable.
 * Immediately after the call returns, the caller must do its own receive or
 * send on the selectable that won. For a queue selectable, call
 * ccol_circq_try_recv_zc(), ccol_dynmq_try_recv_zc(), ccol_circq_try_send_zc(),
 * or ccol_dynmq_send_zc(). For an fd selectable, call read(2), recv(2),
 * write(2), or send(2) on selectables[*ready_index].fd. This contract is
 * TOCTOU-safe, and it is the same for both directions. Another consumer or
 * producer can win the race between the return of ccol_select() and the call
 * of the caller. This is why that call must not block, and why the caller must
 * check its result.
 *
 * One array can hold read selectables and write selectables together. Each
 * selectable in the array can have any direction. The first one that becomes
 * ready wins the race.
 *
 * The function allocates one waiter node on the heap for each selectable. Any
 * number of threads can call ccol_select on the same queue at the same time.
 * There is no fixed limit on the number of waiters. For each message that a
 * producer sends, and for each slot that it frees, the library wakes exactly
 * one waiter. It never wakes all of them together. A woken caller can leave
 * without an action on that queue. This happens when its deadline ends, when
 * another selectable in the same array wins, or when the call fails. Such a
 * caller passes the wake to the next waiter if the queue is still ready for
 * that direction. No waiter therefore stays parked on a queue that holds
 * something for it.
 *
 * This call does the same work as
 * ccol_select_timed(ready_index, n, selectables, UINT64_MAX).
 *
 * @param ready_index The function sets this to the index of the selectable
 *                    that became ready. It is valid only when the function
 *                    returns ccol_success.
 * @param n           Number of selectables. The value must be >= 1 and
 * <= INT_MAX. Inside, the library narrows the index of the matched selectable
 * through a plain int. A larger n is therefore refused, because it could
 * truncate *ready_index. The value must also be no larger than
 * SIZE_MAX / sizeof(waiter node), because this call holds its waiter nodes in
 * one allocation of n nodes. No realistic caller reaches either ceiling.
 * @param selectables Array of n ccol_selectable values to watch
 *
 * @return ccol_success           A selectable is ready, and *ready_index is
 *                                set. The caller must then do its own receive
 *                                or send. See above.
 * @return ccol_invalid_args      An argument is NULL or zero. Or n is too
 *                                large; see the n parameter above. Or a queue
 *                                selectable holds a NULL queue pointer. Or an
 *                                fd selectable has fd < 0. Or the type or the
 *                                dir value is unknown.
 * @return ccol_not_enough_memory The allocation of the waiter nodes failed.
 *                                There is one node for each selectable. The
 *                                function changed no state.
 * @return ccol_unexpected_failure epoll_create1, eventfd, or epoll_ctl failed.
 *                                An fd selectable that names a file that
 *                                epoll(7) cannot watch gives this code on
 *                                every call; see the note below.
 *
 * @note If the array holds no fd selectable, the function uses only POSIX
 *       condition variables. It makes no system call other than the calls of a
 *       normal queue operation.
 * @note If the array holds one fd selectable or more, the function uses
 *       epoll(7) and eventfd(2) inside. Those are available only on Linux.
 *       Every fd must therefore name a file that epoll(7) can watch, such as
 *       a socket, a pipe, a FIFO, a terminal or an eventfd. A regular file
 *       and a directory are refused by epoll_ctl with EPERM, and the call
 *       returns ccol_unexpected_failure; poll(2) and select(2) report such a
 *       file as always ready instead.
 */
ccol_retval_t ccol_select(size_t *ready_index, size_t n,
                          ccol_selectable *selectables);

/**
 * @brief Wait with a timeout until one of n selectables becomes readable or
 * writable
 *
 * This function does the same work as ccol_select(). There is one difference.
 * If no selectable becomes ready in timeout_us microseconds, the call returns
 * ccol_timed_out.
 *
 * @param ready_index The function sets this to the index of the selectable
 *                    that became ready. It is valid when the function returns
 *                    ccol_success. The function does not change it on
 *                    ccol_timed_out, ccol_invalid_args,
 *                    ccol_not_enough_memory, or ccol_unexpected_failure.
 * @param n           Number of selectables. See the n parameter of
 *                    ccol_select for the exact bound.
 * @param selectables Array of n ccol_selectable values to watch
 * @param timeout_us  Maximum time to wait, in microseconds. Pass UINT64_MAX
 *                    to wait with no time limit. This is the behaviour of
 *                    ccol_select. Pass 0 to poll with no block; the poll
 *                    still reports a selectable that is ready at the moment
 *                    of the call, a queue or an fd. Any other value waits
 *                    at least that long before the call reports a timeout.
 *                    A value too large to form a deadline saturates to the
 *                    latest deadline that the clock can hold, and never
 *                    wraps into the past.
 *
 * @return ccol_success           A selectable is ready, and *ready_index is
 *                                set.
 * @return ccol_timed_out         timeout_us ended and no selectable became
 *                                ready. The function does not change
 *                                *ready_index.
 * @return ccol_invalid_args      The same conditions as ccol_select
 * @return ccol_not_enough_memory The same conditions as ccol_select
 * @return ccol_unexpected_failure epoll_create1, eventfd, or epoll_ctl
 *                                failed, as with ccol_select. Or the array
 *                                holds no fd selectable, timeout_us is not
 *                                UINT64_MAX, and the timed wait on the
 *                                condition variable reported an unexpected
 *                                system error.
 *
 * @note The deadline uses CLOCK_MONOTONIC. A change to the system time
 *       therefore does not change the timeout.
 * @note If the array holds no fd selectable, the function uses
 *       pthread_cond_timedwait on a CLOCK_MONOTONIC condition variable. If
 *       the array holds one fd selectable or more, the function gives the
 *       time that is left to each epoll_wait call, rounded up to a whole
 *       millisecond. Once the deadline has passed, it still makes one
 *       epoll_wait call with a timeout of 0 before it reports
 *       ccol_timed_out.
 */
ccol_retval_t ccol_select_timed(size_t *ready_index, size_t n,
                                ccol_selectable *selectables,
                                uint64_t timeout_us);

/**
 * @brief Short form: call ccol_select with the list of selectables in line
 *
 * This macro builds the array of selectables from its variadic arguments. It
 * computes the count at compile time. It gives everything to ccol_select. The
 * result of the macro is the ccol_retval_t that ccol_select returns.
 *
 * Each argument must be a ccol_selectable value. Usually
 * ccol_selectable_from_circq(), ccol_selectable_from_dynq(), or
 * ccol_selectable_from_chan() builds it, with a dir of ccol_select_read or
 * ccol_select_write. The macro evaluates each argument exactly once.
 *
 * Example:
 * @code
 *   size_t idx;
 *   ccol_retval_t r = ccol_select_va(&idx,
 *       ccol_selectable_from_circq(q0, ccol_select_read),
 *       ccol_selectable_from_chan(ch, ccol_select_read));
 *   if (r == ccol_success) {
 *       // Do the receive that is correct for the selectable that won.
 *       // For example, ccol_circq_try_recv_zc(q0, &msg).
 *   }
 * @endcode
 *
 * @note The macro uses a GCC and Clang statement expression. It is not valid
 * under strict ISO C.
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
 * This macro does the same work as ccol_select_va(). It gives timeout_us, in
 * microseconds, to ccol_select_timed.
 *
 * Example:
 * @code
 *   size_t idx;
 *   ccol_retval_t r = ccol_select_timed_va(&idx, 500000,
 *       ccol_selectable_from_circq(q0, ccol_select_read),
 *       ccol_selectable_from_chan(ch, ccol_select_read));
 * @endcode
 *
 * @note The macro uses a GCC and Clang statement expression. It is not valid
 * under strict ISO C.
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
 * This is a reactor that stays alive and that the caller can change one step
 * at a time. It is built on epoll(7). In every configuration, exactly ONE
 * dedicated poller thread calls epoll_wait. This is a deliberate property of
 * the design, not a detail of the implementation. If more than one thread
 * calls epoll_wait on one shared epoll instance, one ready event wakes every
 * blocked thread. That is a true thundering herd at the level of the kernel,
 * and it costs measurable tail latency for a workload with few concurrent
 * operations. One point here is not obvious: EPOLLEXCLUSIVE does NOT help.
 * EPOLLEXCLUSIVE governs one target fd that is registered in several SEPARATE
 * epoll instances. It does not govern many threads that share one instance.
 * See num_reactor_threads on ccol_event_loop_create_with_mprocs. It shows how
 * the loop still gives DISPATCH throughput on many threads while only one
 * thread polls.
 *
 * A registration is built from the same ccol_selectable type that ccol_select
 * uses (ccol_selectable_from_fd, ccol_selectable_from_circq,
 * ccol_selectable_from_dynq, ccol_selectable_from_chan). As with ccol_select,
 * ccol_event_loop never does the receive or the send itself, for any type of
 * selectable. The caller always makes its own call from inside the callback:
 * read(), recv(), ccol_circq_try_recv_zc(), or ccol_dynmq_try_recv_zc(). See
 * the documentation of ccol_event_loop_add.
 *
 * With num_reactor_threads > 1, the callbacks run on separate worker threads
 * and not on the poller. Two correctness properties hold there. A
 * single-threaded reactor gets both of them free. It has only one caller.
 * The two properties are:
 *  - The loop never runs the callbacks of one registration at the same time
 *    as themselves. The rule is stricter than that. A read registration and a
 *    write registration that share one fd also never run at the same time as
 *    each other. A pair of callbacks that shares state across both directions
 *    of one fd therefore needs no lock of its own. One TLS connection object
 *    is such a pair.
 *  - You can remove a registration and use its fd number again for a new,
 *    unrelated registration. This is safe even while a worker thread is still
 *    inside a dispatch that points to the old registration. A dispatch always
 *    checks liveness immediately before it calls a callback. A stale pointer
 *    to a registration that is already removed is therefore a safe no-op. It
 *    is never a use-after-free, and the loop never sends that callback to the
 *    new registration. For the same reason, the loop never collects a second
 *    registration for the same entry while an earlier one is still in
 *    flight. In flight means queued or in execution. Application code can
 *    therefore call ccol_event_loop_modify from inside a callback that is in
 *    flight. That is a supported and common pattern. It does not race a
 *    second dispatch that the loop collects at the same time for that same
 *    registration.
 *
 * A third property holds in every configuration, whatever num_reactor_threads
 * is. Different threads can call ccol_event_loop_modify,
 * ccol_event_loop_pause, ccol_event_loop_resume, ccol_event_loop_remove, and
 * ccol_event_loop_reg_generation at the same time, against the very same
 * ccol_event_reg. For example, one thread can remove a registration while
 * another thread races it and tries to change the same one or ask about it.
 * More generally, a thread can make such a call at any time at all. The time
 * that passed since another thread called ccol_event_loop_remove() on that
 * same registration does not matter. Every use of a ccol_event_reg handle goes
 * through the generation-tagged slot table of that loop before the library
 * dereferences anything. See the doc comment of ccol_event_reg. The library
 * therefore always finds a stale handle, or a handle that a call already
 * removed. It finds it every time, and it is never a use-after-free. The side
 * that loses the race only sees a registration that is already removed. It
 * gets ccol_invalid_args, or generation 0. See ccol_event_loop_reg_generation()
 * for the identity token that this makes available to the caller. A
 * registration can use that token for its own defensive bookkeeping across the
 * reuse of an fd. That is a separate concern from the two guarantees above,
 * which always hold, whether or not a caller ever reads the token.
 */
typedef struct ccol_event_loop_s ccol_event_loop_s;

/**
 * @brief Opaque ccol_event_loop handle.
 *
 * ccol_event_loop is an opaque VALUE handle. It packs a slot index and a
 * generation into one value. It is not a pointer. Never cast it to void* or
 * from void*. Never compare it through a pointer cast. Never treat it as an
 * address. Compare it directly against CCOL_EVENT_LOOP_INVALID. You can also
 * use it in a truthiness check, because CCOL_EVENT_LOOP_INVALID is 0. The test
 * `if (!loop)` is therefore a correct test for "no loop". Inside, the library
 * resolves every use of a ccol_event_loop through a slot table that the
 * library owns. It does this before it touches the struct ccol_event_loop_s*
 * below the handle. The library therefore always finds a handle whose slot is
 * already free, or whose slot now holds an unrelated, later loop. It never
 * dereferences freed memory or the memory of the wrong object. See the doc
 * comment of ccol_event_loop_destroy for what that function does with a stale
 * handle.
 */
typedef uint64_t ccol_event_loop;

/** @brief Sentinel value for "no loop". It is the ccol_event_loop equivalent
 * of NULL.
 */
#define CCOL_EVENT_LOOP_INVALID ((ccol_event_loop)0)

/**
 * @brief Opaque handle to a single ccol_event_loop registration.
 *
 * ccol_event_reg is an opaque VALUE handle. It packs a slot index and a
 * generation into one value, and it belongs to the one ccol_event_loop that
 * gave it. It is not a pointer. Never cast it to void* or from void*. Never
 * compare it through a pointer cast. Never treat it as an address. Compare it
 * directly against CCOL_EVENT_REG_INVALID. You can also use it in a truthiness
 * check, because CCOL_EVENT_REG_INVALID is 0. Inside, the library resolves
 * every use of a ccol_event_reg through the reg slot table of that loop. It
 * does this before it touches the struct below the handle. The library
 * therefore always finds a handle whose slot is already free. That slot
 * becomes free when ccol_event_loop_remove runs, from this thread or from
 * another thread that races it. The matching entry point then returns
 * ccol_invalid_args, or 0 for ccol_event_loop_reg_generation. It is never a
 * use-after-free. This is the same handle design as ccol_event_loop, for one
 * loop instead of the whole process.
 */
typedef uint64_t ccol_event_reg;

/** @brief Sentinel value for "no registration". It is the ccol_event_reg
 * equivalent of NULL. */
#define CCOL_EVENT_REG_INVALID ((ccol_event_reg)0)

/**
 * @brief Callback that the loop calls when a registration becomes readable
 *
 * The callback reports readiness only, for every type of selectable. The
 * reactor never does the receive itself. For an fd selectable, the callback
 * makes its own read() or recv() call on sel->fd. A queue selectable is a
 * circq, a dynq, or a queue that comes from a channel. For such a
 * selectable, the callback makes its own ccol_circq_try_recv_zc() or
 * ccol_dynmq_try_recv_zc() call on sel->cq or sel->dq. For a queue that comes
 * from a channel, that is ccol_circq_try_recv_zc() on sel->cq, and never
 * ccol_chan_try_recv_zc(); see ccol_selectable_from_chan. In both cases
 * another consumer can already hold the data. That TOCTOU race is part of the
 * design, and it is the same contract as the write direction of ccol_select.
 * The receive call of the callback can therefore find nothing, and the
 * callback must handle that well.
 *
 * For a queue selectable, one notification does not always mean exactly one
 * message. Several sends can run faster than the dispatch, and the callback
 * then sees them as one notification. A callback that receives one message
 * for each call still drains a burst: while the queue stays ready after a
 * callback that moved at least one message, the loop dispatches the
 * listeners again. A callback can also receive in a loop until the call
 * returns ccol_container_empty, which drains a burst in fewer dispatches.
 *
 * Every callback of the three dispatch types (this one,
 * ccol_event_writable_fn and ccol_event_error_fn) receives the handle of its
 * own registration in reg. It is exactly the value that ccol_event_loop_add
 * returns for that registration, and the callback can pass it to
 * ccol_event_loop_remove, ccol_event_loop_pause, ccol_event_loop_resume,
 * ccol_event_loop_modify and ccol_event_loop_reg_generation. This holds even
 * for a dispatch that runs before ccol_event_loop_add has returned to its
 * caller, which can happen with num_reactor_threads > 1 when the selectable
 * is already ready at registration time. A callback therefore never needs to
 * read the handle from storage that the adding thread writes after the call
 * returns.
 *
 * @param loop The ccol_event_loop that owns this registration
 * @param reg  The handle of this registration, as ccol_event_loop_add returns
 *             it
 * @param sel  The ccol_selectable that this registration comes from. The
 *             value of sel->dir is the current direction of the
 *             registration. A ccol_event_loop_modify call can change that
 *             direction after ccol_event_loop_add.
 * @param arg  The opaque pointer that the caller gave to ccol_event_loop_add
 */
typedef void (*ccol_event_readable_fn)(ccol_event_loop loop, ccol_event_reg reg,
                                       ccol_selectable *sel, void *arg);

/**
 * @brief Callback that the loop calls when a registration becomes writable
 *
 * The callback never gets a payload. For an fd selectable, the caller makes
 * its own write() or send() call. For a queue selectable, the registration
 * only signals that the queue can have room. The callback must then call
 * ccol_circq_try_send_zc or ccol_dynmq_send_zc itself. For a queue that comes
 * from a channel, that is ccol_circq_try_send_zc on sel->cq, and never
 * ccol_chan_try_send_zc; see ccol_selectable_from_chan. A TOCTOU race is
 * possible. This is the same contract as a win on the write direction of
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
 * For an fd selectable, this callback runs for each condition that the
 * direction of the registration has no other handler for. EPOLLERR and
 * EPOLLHUP go here whenever the direction is not also ready. For a
 * read-direction registration with on_readable == NULL, the close or the
 * shutdown of the end of the peer also goes here. That is EPOLLRDHUP, which
 * an ordinary TCP close() raises without EPOLLHUP.
 *
 * If the direction does have its own handler, that handler wins the same
 * event whenever the direction is also ready. Data can arrive together with
 * the error. A read-direction registration with on_readable set then gets
 * on_readable, and not on_error. The loop therefore still delivers the bytes
 * that arrived before the peer went away. For the same reason, a
 * write-direction registration with on_writable set gets on_writable. If both
 * directions are registered on one fd, the loop judges each direction alone,
 * and each one can get an error dispatch. A queue selectable never makes an
 * error condition, and it never calls this callback.
 *
 * A registration with on_error == NULL gets EPOLLERR and EPOLLHUP through the
 * handler of its own direction instead, as libevent and libuv deliver them:
 * on_readable for a read registration, on_writable for a write registration.
 * The read(), recv(), write() or send() of that handler then reports the
 * condition: -1 with the error in errno, or 0 for an end of file. A read or
 * a write of a socket also clears a pending socket error. A transient error,
 * such as the ICMP port unreachable that a connected UDP socket reports as
 * ECONNREFUSED, therefore costs one failed receive, and the datagrams that
 * arrive after it still reach on_readable. When both directions of one fd
 * have such a handler, one event reaches both: on_readable first, then
 * on_writable, one after the other on one thread, and each sees whatever the
 * other left of the condition.
 *
 * @note A registration can carry on_error alone. It leaves the on_readable or
 * on_writable of its own direction NULL. This is how a caller says: tell me
 * when this fd dies, and I do not want to read it or write to it. Both
 * directions support this shape fully. The loop never arms such a
 * registration for the readiness that it has no handler for. A healthy socket
 * is writable from the moment of its registration. An error-only registration
 * on the write direction therefore costs nothing until something goes wrong.
 *
 * @warning An error is level-triggered, as every other condition here is. The
 * loop reports it again and again, and it calls on_error, or the handler of
 * the direction when on_error is NULL, again and again. This continues until
 * a call removes or pauses the registration, or until the condition clears. A
 * hang-up stays true: a pipe whose writer closed, or a socket whose peer is
 * gone, keeps reporting it, and a read() of it keeps returning 0. The loop
 * therefore calls a handler many times if that handler neither removes the
 * registration nor clears the condition in another way. A handler that reads an
 * end of file must remove the registration (or pause it), exactly as it must
 * for any other readiness that it leaves in place. Remove it before you close
 * the fd; see ccol_event_loop_remove.
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
 * The loop calls this callback exactly once for each registration that it
 * really put into an ccol_event_loop. It never calls it for a
 * ccol_event_loop_add call that failed. There are two occasions for the call.
 * The first occasion has two parts. A thread calls ccol_event_loop_remove()
 * for this registration. That thread can be any thread, and it can even be
 * inside one of the on_readable, on_writable, or on_error callbacks of this
 * same registration. Then every dispatch of the registration that was in
 * flight, or that the loop already collected for dispatch at that moment,
 * finishes. The second occasion is a destroy of the owning ccol_event_loop
 * while this registration is still live.
 *
 * This is the only one of the four callback types that is asynchronous. The
 * loop never calls it from inside ccol_event_loop_remove(). It can run some
 * time after that call returns. That time is bounded. With
 * num_reactor_threads == 1, it runs at the next point between two batches on
 * the reactor thread. See the note of ccol_event_loop_remove(). That note
 * explains why the caller cannot free the memory of arg as soon as the
 * removal returns. ccol_event_loop_destroy() is different: it calls every
 * on_removed that is still due from inside the call, before it returns. That
 * covers a registration that is still live at the destroy, and a removed one
 * whose on_removed has not run yet.
 *
 * The callback runs on the thread that does the reclamation. For an ordinary
 * removal, that is the reactor thread. For a registration that is still live
 * at a destroy, it is the thread that called ccol_event_loop_destroy. That
 * thread holds none of the internal locks of this module. It is therefore
 * always safe for on_removed to take an application lock of its own. This is
 * true even for a lock that the other callbacks of this registration also
 * take.
 *
 * A call of ccol_event_loop_destroy() on the owning loop from on_removed is
 * fatal (ccol_fatal_err()). On an ordinary removal on_removed runs on the
 * reactor thread, where a destroy of its own loop is refused, and during a
 * destroy the handle is already destroyed.
 *
 * A registration can set on_removed to NULL. It then gets the same contract as
 * every other callback type. There is no promise about the moment when a
 * stale callback that is still in flight finishes with arg. The only promise
 * is the weaker one in the documentation of ccol_event_loop_remove(), which is
 * weaker by design.
 *
 * @param arg The opaque pointer that the caller gave to ccol_event_loop_add
 */
typedef void (*ccol_event_removed_fn)(void *arg);

/**
 * @brief Group of callbacks for one ccol_event_loop_add call
 *
 * Each of on_readable, on_writable, and on_error can be NULL. For an fd
 * selectable, the loop does not arm a direction that has no handler of its
 * own. A NULL on_readable or on_writable therefore costs no dispatch at all.
 * The loop does not make a dispatch that nothing can use. A condition that
 * reaches such a direction goes to on_error. The close of the peer and an
 * error are two such conditions. With on_error NULL, an error or a hang-up
 * goes to the handler of the direction instead; see ccol_event_error_fn. For
 * example, a producer that only writes can pass NULL for on_error and see an
 * error as the failure of its own write() in on_writable.
 *
 * An fd event that no handler of the registration can take is dropped, and
 * the loop also stops watching that registration: a hang-up can stay true,
 * and a level-triggered epoll would report it again on every wait. Only a
 * registration with neither the handler of its direction nor on_error gets
 * there, for example a read registration with both on_readable and on_error
 * NULL whose peer hangs up. The registration stays registered, and a
 * registration for the other direction of the same fd stays watched.
 * ccol_event_loop_resume() and a successful ccol_event_loop_modify() arm it
 * again, whether the modify changes the direction or keeps it.
 *
 * on_removed can also be NULL. Some args need no notice at all before the
 * caller frees them. An arg on the stack is one example. An arg that the
 * program owns statically is another. A caller can also have its own way to
 * know when a free is safe. For example, the registration can destroy arg well
 * after ccol_event_loop_remove() returns, and not at the moment when it
 * returns.
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
 * This function creates an epoll instance that stays alive. It immediately
 * starts the threads that drive it. Every thread is ready to dispatch events
 * as soon as this call returns. ccol_create_cthread_pool has the same
 * ergonomics: the pool is ready to work the moment that you get the handle.
 *
 * num_reactor_threads == 1 gives a pure single-thread reactor. That one thread
 * calls epoll_wait AND runs every callback in line. num_reactor_threads > 1
 * starts exactly ONE dedicated thread that calls epoll_wait. There is never
 * more than one. The doc comment of the ccol_event_loop struct explains why.
 * The loop then also starts (num_reactor_threads - 1) worker threads, and
 * those threads run the callbacks. The total OS thread count for a given
 * num_reactor_threads is therefore always that exact value. The parameter
 * keeps one meaning for resource use in both configurations.
 *
 * In the first configuration, the callback of a registration runs on the
 * poller thread. In the second configuration, it runs on one of the worker
 * threads. Callback code cannot see the difference, because
 * ccol_event_readable_fn, ccol_event_writable_fn, and ccol_event_error_fn have
 * no way to read it. There is one rule for both kinds of thread: do not call
 * ccol_event_loop_shutdown from inside a callback. That is a self-join hazard.
 * See the doc comment of ccol_event_loop_shutdown.
 *
 * num_reactor_threads == 1 costs exactly what a plain single-threaded loop
 * costs. The loop never creates the dispatch pool, so the code path is the
 * same path. Above one thread, one dedicated poller is what keeps tail latency
 * low when few operations run together. N threads that all call epoll_wait on
 * the same set of fds make a true kernel thundering herd instead. That is not
 * a property of the code of this module; see the doc comment of the
 * ccol_event_loop struct. The separate dispatch pool keeps the total
 * multi-threaded dispatch throughput under real concurrent load.
 *
 * The registry of fds and entries uses lock striping. It holds
 * num_lock_stripes independent pairs of a mutex and a chmap. Each pair guards
 * its own subset of the registrations, and the subsets do not overlap. Exactly
 * one stripe handles one real fd. It also handles the private bridge fd of one
 * queue registration or one ccol_channel registration. The library never
 * splits one of them across two stripes. A value of 1 gives one registry lock
 * for the whole loop. That costs only one more step through an array. A larger
 * value lets ccol_event_loop_add, ccol_event_loop_remove, and
 * ccol_event_loop_modify calls for different fds and registrations run
 * together. They then do not go one after the other through one lock. The cost
 * is num_lock_stripes mutexes and chmaps, which the loop allocates at the
 * start. This is independent of num_reactor_threads. The stripe count controls
 * contention on the registry. The thread count controls how much dispatch runs
 * together.
 *
 * @param max_events_per_wait Size of the epoll_wait batch buffer of the poller
 * thread. The value must be from 1 to INT_MAX. It must also be small enough
 * that max_events_per_wait * sizeof(struct epoll_event) does not overflow
 * size_t. This function returns CCOL_EVENT_LOOP_INVALID for a value outside
 * that range. The
 * library narrows the value to the int maxevents parameter of epoll_wait, and
 * it sizes the events buffer of the poller thread from it. The value bounds
 * how many ready events one epoll_wait call drains. It does not bound how many
 * registrations the loop can hold.
 * @param num_lock_stripes Number of independent lock stripes for the registry
 * of fds and entries. The value must be >= 1. A value of 1 means one registry
 * lock for the whole loop. Pass a larger value to lower the contention of
 * ccol_event_loop_add, ccol_event_loop_remove, and ccol_event_loop_modify
 * across many different fds and registrations under concurrent use. There is
 * no upper bound.
 * @param num_reactor_threads Total number of OS threads for the polling and
 * the dispatch of this loop. The value must be >= 1. A value of 1 means a pure
 * single-thread reactor, where one thread polls and dispatches in line. Every
 * larger value means exactly one dedicated polling thread plus
 * (num_reactor_threads - 1) dispatch worker threads. See the doc comment of
 * the ccol_event_loop struct for the two correctness guarantees. They hold for
 * every value.
 * @param mmgmt_procs Custom memory management procedures, or NULL to use the
 * default malloc and free
 * @param err_str Optional pointer that gets an error string on failure. Pass
 * NULL to ignore it.
 *
 * @return New ccol_event_loop handle, or CCOL_EVENT_LOOP_INVALID on failure.
 * The call fails on arguments that are not valid, on a failed allocation, and
 * on a failure of epoll_create1, eventfd, or pthread_create. It also fails
 * when the process has no thread-specific key left for the two keys that the
 * dispatch of every loop uses; the library creates them once for each
 * process and does not try again. And it fails once the process-exit
 * destructors of the library have run and the last loop is gone, for
 * example from an application destructor that runs after them.
 *
 * @see ccol_event_loop_create
 * @see ccol_event_loop_destroy
 */
ccol_event_loop ccol_event_loop_create_with_mprocs(
    size_t max_events_per_wait, size_t num_lock_stripes,
    size_t num_reactor_threads, ccol_memmgmt_procs_t *mmgmt_procs,
    char **err_str);

/**
 * @brief Create a ccol_event_loop with default memory management
 *
 * This macro is the short form of ccol_event_loop_create_with_mprocs with
 * mmgmt_procs = NULL.
 */
#define ccol_event_loop_create(max_events_per_wait, num_lock_stripes, \
                               num_reactor_threads, err_str)          \
  ccol_event_loop_create_with_mprocs((max_events_per_wait),           \
                                     (num_lock_stripes),              \
                                     (num_reactor_threads), NULL, (err_str))

/**
 * @brief Register a selectable with the event loop
 *
 * This function uses the same ccol_selectable type as ccol_select. You can
 * build sel directly with ccol_selectable_from_fd,
 * ccol_selectable_from_circq, ccol_selectable_from_dynq, or
 * ccol_selectable_from_chan.
 *
 * One registration covers exactly one direction, which is sel.dir. A caller
 * can want both directions live on one fd at the same time. A full-duplex pipe
 * is one example. Such a caller calls ccol_event_loop_add two times and gets
 * two independent handles. A caller can also want to change the direction of
 * one registration over time. A socket that connects is one example: it wants
 * write interest until the connect finishes, and read interest after that.
 * Such a caller uses one registration and ccol_event_loop_modify. The function
 * refuses a second selectable for a direction that is already in use on the
 * same fd. Two read registrations on one fd is one such case. The call then
 * returns CCOL_EVENT_REG_INVALID, and err_str names that cause; see the
 * return value below.
 *
 * A queue selectable and a ccol_channel selectable have no such restriction.
 * You can make any number of ccol_event_loop_add calls for the same queue and
 * the same direction, in one ccol_event_loop or in several. You can also mix
 * those with a ccol_select() call that runs at the same time on the same queue
 * and the same direction. All of them are live together, and each one gets a
 * turn. This is the same guarantee that ccol_select gives: any number of
 * threads can watch one queue at the same time.
 *
 * A message can arrive while more than one such listener is registered. For
 * each producer call and each consumer call, the library wakes exactly one
 * listener. It never wakes all of them together, because that would be a
 * thundering herd. After the callback of the woken listener returns, that
 * listener checks whether the queue is still ready for its direction. If it
 * is, the listener passes the wake to the next listener in the line. A
 * ccol_select() caller that wakes and then leaves without an action on the
 * queue passes the wake in the same way, and so does a registration that is
 * removed, or whose loop is destroyed, before it acted on its wake. No wake
 * is therefore lost, and the library never passes over a live listener for
 * ever.
 *
 * A registration whose callback moved at least one message is dispatched
 * again for as long as the queue stays ready for its direction: for a read
 * registration the queue holds fewer messages after the callback than
 * before it, and for a write registration it holds more. This holds for
 * each registration on its own, wherever it sits in the line and whatever
 * the other listeners of the queue do. A backlog therefore drains without
 * another send or receive, even when each callback receives one message,
 * and a listener that declines the queue never holds up one that takes it.
 * A registration whose callback neither receives nor sends passes the wake
 * on, and a wake that every listener declines stops once it has visited
 * each of them, so listeners that ignore a ready queue do not spin. A write
 * registration whose callback sends on every call is dispatched again for
 * as long as the queue has room, exactly as a writable fd is; remove it when
 * it has nothing to send.
 *
 * The registration is live, and a callback of it can run, before this call
 * returns. With num_reactor_threads > 1 a callback can run on a dispatch
 * worker while the calling thread is still inside this function, for example
 * when the fd is already ready. Each callback receives the handle of its own
 * registration as its reg parameter, so it never has to read the return value
 * of this call from storage that the calling thread writes afterwards.
 *
 * @param loop     ccol_event_loop to register with
 * @param sel      What to watch. See above.
 * @param handlers Group of callbacks. Each callback can be NULL.
 * @param arg      Opaque pointer that the loop gives to every callback of
 *                 this registration
 * @param err_str  Optional pointer that gets an error string on failure, and
 *                 NULL on success
 *
 * @return New registration handle, or CCOL_EVENT_REG_INVALID on failure. On
 * failure *err_str, when err_str is not NULL, names the cause, one message
 * for each class. A direction that another registration on the same fd
 * already holds gives a message that contains "direction already in use";
 * no retry can succeed there. A failed allocation gives a message that
 * contains "failed to allocate", and a failed system call (epoll_ctl,
 * eventfd, or the initialisation of a mutex) gives one that contains "a
 * system call failed". An fd that names a regular file or a directory
 * always fails in that last way, because epoll(7) cannot watch such a file
 * (epoll_ctl fails with EPERM). An invalid or stale loop handle and
 * malformed arguments have messages of their own.
 *
 * @note The function is thread-safe. You can call it at the same time as
 *       ccol_event_loop_remove and ccol_event_loop_modify. You can also call
 *       it from inside a callback that runs on the reactor thread.
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
 * The value only goes up, and it is unique across the whole loop. The library
 * makes it once, when it first registers the fd that reg belongs to. That fd
 * can also be the bridge fd of a queue or a ccol_channel. The new-entry path
 * of ccol_event_loop_add makes the value. Every registration on that same fd
 * shares the value for all the time that the fd lives. The value survives a
 * ccol_event_loop_modify call, because a change of direction keeps the same fd
 * and the same connection. A read registration and a write registration on one
 * fd also share one generation, because they are one logical connection.
 *
 * This token is here for the defensive bookkeeping of a caller across the
 * reuse of an fd. A caller can hold a connection object across several
 * asynchronous steps. It can stamp that object with the generation that it
 * reads immediately after ccol_event_loop_add returns. Later, it can read the
 * generation again and compare the two values. The comparison shows whether
 * the caller still works with the same logical connection. The token is not
 * necessary for correctness. The dispatch of ccol_event_loop always checks the
 * liveness of a registration before it calls any callback. It does this every
 * time, whether or not a caller ever calls this function. See the doc comment
 * of the ccol_event_loop struct.
 *
 * You can call this function at any time, on any ccol_event_reg value that
 * ccol_event_loop_add ever gave for this loop. This includes a value that a
 * call already removed. That call can be an earlier call on this thread, or a
 * call on another thread that races this one right now. The library resolves
 * reg through the generation-tagged slot table of the loop before it
 * dereferences anything. It therefore always finds a stale handle, or a handle
 * that a call already removed, and it reports generation 0. It is never a
 * use-after-free.
 *
 * @param loop ccol_event_loop that owns reg
 * @param reg Registration to ask about
 * @return The generation value, or 0. The function gives 0 in four cases.
 * reg is CCOL_EVENT_REG_INVALID. A call already removed reg. loop is
 * CCOL_EVENT_LOOP_INVALID. Or loop is a stale handle, or a handle that a
 * call already destroyed. The counter starts at 1, so 0 is never a valid
 * generation for a real registration.
 */
uint64_t ccol_event_loop_reg_generation(ccol_event_loop loop,
                                        ccol_event_reg reg);

/**
 * @brief Change the direction of an fd registration that already exists
 *
 * This function works only on an fd. On a registration that comes from a queue
 * selectable or a ccol_channel selectable, it returns ccol_invalid_args. The
 * direction of a queue selectable is part of its identity. Remove such a
 * registration and add it again instead.
 *
 * On success, the function changes the ccol_selectable.dir of the
 * registration. The callbacks that come after that see the new value in the
 * sel parameter. The function moves the registration to the other slot of its
 * fd. It computes the combined epoll interest mask of the fd again. There is
 * no moment where the fd is not registered. A reg that the loop stopped
 * watching because an event reached it that none of its handlers can take
 * (see ccol_event_handlers_t) is watched again in its new direction. A call
 * whose new_dir is the current direction of reg changes nothing else, and
 * it also makes such a reg watched again.
 *
 * @param loop    ccol_event_loop that owns the registration
 * @param reg     Registration to change
 * @param new_dir ccol_select_read or ccol_select_write
 *
 * @return ccol_success on success
 * @return ccol_invalid_args in these cases. loop is CCOL_EVENT_LOOP_INVALID,
 * or loop is a stale handle or a handle that a call already destroyed. Or reg
 * is CCOL_EVENT_REG_INVALID, or reg does not resolve for another reason. A
 * call on this thread or on another thread that races it can already have
 * removed reg. Or reg is a queue registration or a ccol_channel registration.
 * Or new_dir is not valid.
 * @return ccol_not_permitted if another registration on the same fd already
 * holds the target direction
 *
 * @note The function is thread-safe. You can call it at the same time as
 *       ccol_event_loop_remove. You can also call it from inside a callback
 *       that runs on the reactor thread.
 */
ccol_retval_t ccol_event_loop_modify(ccol_event_loop loop, ccol_event_reg reg,
                                     ccol_select_dir new_dir);

/**
 * @brief Stop the delivery of events for an fd registration for a time, and
 *        keep the registration
 *
 * This function works only on an fd. It has the same restriction as
 * ccol_event_loop_modify. ccol_event_loop_remove unregisters reg fully and
 * then defers the free. ccol_event_loop_pause is different: it keeps reg
 * whole. The registration keeps its slot on the entry of the fd. It still
 * counts in ccol_event_loop_reg_count. It still carries the same
 * ccol_event_loop_reg_generation. The function only computes the combined
 * epoll interest mask of the fd again and leaves reg out of it. While reg is
 * paused, no on_readable, on_writable, or on_error callback runs for it. This
 * is the same as a removal. The other direction on the same fd, if there is
 * one, does not change.
 *
 * Some callers bring the same logical registration back later. For them, this
 * function is the cheap answer to a ccol_event_loop_remove plus a later
 * ccol_event_loop_add. One example is a connection that goes to a worker
 * thread for body I/O that blocks. That connection then comes back to the
 * reactor for its next request. The pause needs no allocation on the heap
 * and no free. It also makes no churn in the chmap of the fd registry. With
 * one reactor thread,
 * that is num_reactor_threads == 1, it costs one epoll_ctl call. A
 * remove-then-add pair costs two calls, DEL and then ADD.
 *
 * With more than one reactor thread, the dispatch worker still re-arms
 * EPOLLONESHOT one more time after a paused callback returns. That is
 * harmless. The internal re-arm helper of ccol_event_loop always computes the
 * mask again from live state. An extra re-arm therefore applies the same mask,
 * which still leaves reg out. It brings back no race. Pause and resume also
 * avoid the allocation and the registry churn in that configuration.
 *
 * A pause of a reg that is already paused succeeds and does nothing.
 *
 * @param loop ccol_event_loop that owns the registration
 * @param reg  Registration to pause
 *
 * @return ccol_success on success
 * @return ccol_invalid_args in these cases. loop is CCOL_EVENT_LOOP_INVALID,
 * or loop is a stale handle or a handle that a call already destroyed. Or reg
 * is CCOL_EVENT_REG_INVALID, or reg does not resolve for another reason. A
 * call on this thread or on another thread that races it can already have
 * removed reg. Or reg is a queue registration or a ccol_channel registration.
 *
 * @note The function is thread-safe. You can call it at the same time as
 *       ccol_event_loop_remove. You can also call it from inside a callback
 *       that runs on the reactor thread.
 *
 * @see ccol_event_loop_resume
 * @see ccol_event_loop_remove
 */
ccol_retval_t ccol_event_loop_pause(ccol_event_loop loop, ccol_event_reg reg);

/**
 * @brief Start the delivery of events again for a registration that
 *        ccol_event_loop_pause paused
 *
 * This function computes the combined epoll interest mask of the fd again and
 * puts reg back into it. It also arms again a reg that the loop stopped
 * watching because an event reached it that none of its handlers can take;
 * see ccol_event_handlers_t. A resume of a reg that is neither paused nor
 * stopped in that way succeeds and does nothing. This covers a reg that no
 * call ever paused, and a reg that a call already resumed. This matches the
 * same rule in ccol_event_loop_modify: a call that asks for the state that the
 * object already has does nothing and succeeds.
 *
 * @param loop ccol_event_loop that owns the registration
 * @param reg  Registration to resume
 *
 * @return ccol_success on success
 * @return ccol_invalid_args in these cases. loop is CCOL_EVENT_LOOP_INVALID,
 * or loop is a stale handle or a handle that a call already destroyed. Or reg
 * is CCOL_EVENT_REG_INVALID, or reg does not resolve for another reason. Or
 * reg is a queue registration or a ccol_channel registration. For example, the
 * connection can close while the caller still believes that it owns a paused
 * registration to resume.
 *
 * @note The function is thread-safe. You can call it at the same time as
 *       ccol_event_loop_remove. You can also call it from inside a callback
 *       that runs on the reactor thread.
 *
 * @see ccol_event_loop_pause
 */
ccol_retval_t ccol_event_loop_resume(ccol_event_loop loop, ccol_event_reg reg);

/**
 * @brief Remove the registration of a selectable from the event loop
 *
 * You can call this function from inside a callback of the very registration
 * that you remove. A registration that removes itself on an error is a common
 * pattern. You can also call it from any other thread. You can even call it
 * while a dispatch for the same registration is in flight. After this call
 * returns, the loop never calls a callback for this registration again. This
 * is also true for a callback that the loop already collected but did not
 * start. With num_reactor_threads > 1, such a callback sits in the queue of a
 * dispatch worker thread.
 *
 * But this call does not wait for a dispatch that is in flight at the moment
 * of the call. Some callers must not free the memory that arg points to while
 * that dispatch still runs. That is the common case for an arg that is not
 * fully self-contained, for example an arg that a callback dereferences. Such
 * a caller needs the on_removed callback in ccol_event_handlers_t, and it sets
 * that callback in the ccol_event_loop_add call. on_removed runs
 * asynchronously, at the moment when the free is provably safe.
 * ccol_event_loop_remove itself never blocks on on_removed and never blocks on
 * that dispatch in flight. A block there would risk a cycle in the lock order.
 * The callback of that dispatch can need an application lock while the caller
 * of this function holds a different one.
 *
 * Some callers are sure that no callback can be in flight at the moment of the
 * removal. One example is a removal from inside the callback of that exact
 * registration. Another is a registration that the loop never dispatched. Such
 * a caller can free arg immediately and needs no on_removed at all.
 *
 * The function does not close an fd, and it does not change the lifetime of a
 * queue. It frees only the registration bookkeeping of the ccol_event_loop.
 * This matches the contract of ccol_selectable_from_fd: the caller owns the
 * fd.
 *
 * Remove every registration of an fd before you close that fd, as libevent
 * and libuv also require. Once the last of them is removed, the loop has
 * already stopped watching the fd when this call returns, and the fd number
 * is free: a new registration of the same fd, or of a new descriptor that
 * reuses the number, is independent of the removed one, and nothing that
 * the loop still does for the removed registration (a dispatch in flight, or
 * the later release of its bookkeeping) changes how the new one is watched.
 * A registration whose fd is closed first still holds the number until it is
 * removed. A ccol_event_loop_add of a descriptor that reuses the number
 * meanwhile fails with ccol_not_permitted for the direction that the stale
 * registration holds, and it can fail with ccol_unexpected_failure for the
 * other direction.
 *
 * For a queue selectable or a ccol_channel selectable, the loop finishes its
 * own bookkeeping for that queue before this call returns. It unlinks the
 * waiter of the registration from the queue, and it frees the registration
 * bookkeeping. A dispatch that is in flight is a separate matter. Its callback
 * holds the selectable that the loop gave it, and it reaches the queue through
 * that selectable. For the queue, the caller needs no on_removed handshake,
 * which is different from arg above. A destroy of the queue waits for any such
 * callback to finish with the queue. See ccol_circular_queue_destroy. The
 * ordinary sequence is therefore safe: drain, remove, destroy. It is safe from
 * any thread that does not itself run one of the callbacks of this
 * registration, and that includes a callback of another registration on the
 * same loop.
 *
 * @param loop ccol_event_loop that owns the registration
 * @param reg  Registration to remove
 *
 * You can call this function more than once on the same reg. The calls can be
 * one after the other, or they can truly run at the same time on different
 * threads. Neither case is ever a use-after-free. But the two cases return
 * different values.
 *
 * A call can start strictly after an earlier ccol_event_loop_remove() on the
 * same reg already returned. For that call, reg_h no longer resolves at all,
 * because the earlier call already freed the slot of reg before it returned.
 * The call gets ccol_invalid_args. That is the same result that
 * ccol_event_loop_modify(), ccol_event_loop_pause(), ccol_event_loop_resume(),
 * and ccol_event_loop_reg_generation() document for a reg that a call already
 * removed.
 *
 * A call that truly races another ccol_event_loop_remove() on the same reg is
 * different. Its own resolve can still succeed, because the other call did not
 * free the slot of reg yet. That window is narrow. Such a call sees that reg
 * already carries the removed mark. It does nothing and returns ccol_success.
 *
 * @return ccol_success on success. This includes the call that loses the race
 * in the window above.
 * @return ccol_invalid_args in these cases. loop is CCOL_EVENT_LOOP_INVALID,
 * or loop is a stale handle or a handle that a call already destroyed. Or reg
 * is CCOL_EVENT_REG_INVALID, or reg does not resolve for another reason. An
 * earlier call that already returned can have removed it.
 *
 * @note The function is thread-safe
 *
 * @see ccol_event_removed_fn
 */
ccol_retval_t ccol_event_loop_remove(ccol_event_loop loop, ccol_event_reg reg);

/**
 * @brief Number of ccol_event_reg handles that are registered now
 *
 * The function counts live registrations. A live registration comes from a
 * ccol_event_loop_add call that no call removed yet. The function does not
 * count the entries of the epoll interest list. One fd with both directions
 * registered counts as 2.
 *
 * @param loop ccol_event_loop to ask
 * @return Count of registrations, or (size_t)-1. The function gives
 * (size_t)-1 if loop is CCOL_EVENT_LOOP_INVALID, or if loop is a stale handle
 * or a handle that a call already destroyed.
 */
size_t ccol_event_loop_reg_count(ccol_event_loop loop);

/**
 * @brief Stop the poller thread of the reactor and its dispatch worker
 *        threads, if it has any
 *
 * You can call this function more than once. You can also never call it before
 * ccol_event_loop_destroy, because that macro calls it inside when it needs
 * to. The function blocks until it joins the poller thread. With
 * num_reactor_threads > 1, it also blocks until every dispatch worker finishes
 * its current job and it joins that worker too. This is a graceful drain and
 * not a cancel. After this call returns, no dispatch can be in flight. That is
 * the same guarantee that num_reactor_threads == 1 gives.
 *
 * @param loop ccol_event_loop to shut down
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop is CCOL_EVENT_LOOP_INVALID, or if loop is
 * a stale handle or a handle that a call already destroyed
 * @return ccol_not_permitted if the call comes from inside a callback that
 * runs on one of the threads of this loop. That thread is the poller, or, for
 * num_reactor_threads > 1, a dispatch worker. See the warning below.
 *
 * @warning Do not call this from inside a callback that runs on one of the
 * threads of this loop. Such a call joins that thread from itself, which is
 * undefined behaviour and gives EDEADLK. The function finds that case and
 * refuses it with ccol_not_permitted. It does not leave undefined behaviour
 * that a caller can trigger. Move the shutdown to another thread, or do it
 * after the callback returns.
 */
ccol_retval_t ccol_event_loop_shutdown(ccol_event_loop loop);

/**
 * @brief Internal destroy. Use the ccol_event_loop_destroy() macro.
 *
 * This function shuts the reactor thread down, if a call did not shut it down
 * already. It then frees every registration that is left.
 *
 * The shutdown is the same graceful drain as ccol_event_loop_shutdown(). The
 * callbacks that the loop already collected still run, and loop stays a live
 * handle for them until the drain ends. A callback can therefore call
 * ccol_event_loop_remove(), ccol_event_loop_pause(),
 * ccol_event_loop_resume(), ccol_event_loop_modify() and
 * ccol_event_loop_add() on loop during the destroy, with the same results as
 * during ccol_event_loop_shutdown(). The documented way to close a
 * connection from its own callback (remove every registration of it, close
 * the fd, free arg) is thus safe while another thread destroys the loop: once
 * the removals return, no callback of those registrations runs again. Once
 * the drain ends, loop stops resolving, and every later call on it fails
 * as for a destroyed handle. For a registration
 * that a queue backs, it first unlinks the registration from the waiter list
 * of that queue. A queue that lives longer than this ccol_event_loop is
 * therefore never left with a waiter pointer that points nowhere.
 *
 * The caller is responsible for the reverse order. Take a queue that a call
 * registered with ccol_selectable_from_circq, ccol_selectable_from_dynq, or
 * ccol_selectable_from_chan. Before the caller destroys that queue, it must
 * remove the registration with ccol_event_loop_remove(). As an alternative,
 * every ccol_select() call and every ccol_select_timed() call that watches the
 * queue must return first. A destroy of the queue before that is a caller bug.
 * ccol_circular_queue_destroy(), ccol_dynamic_queue_destroy(), and
 * ccol_channel_destroy() find that bug and assert against it. Without that
 * assert, the waiter node is still linked and points to the mutex of the
 * queue, which the destroy is about to free.
 *
 * loop must be a handle that is live now. ccol_event_loop_create or
 * ccol_event_loop_create_with_mprocs gave it, and no call destroyed it yet. A
 * stale handle is a fatal error. A handle is stale in two cases. An earlier,
 * finished call to this same function destroyed it. Or another thread
 * destroys it in a race with this call right now. A forged value or garbage
 * is a fatal error too.
 * This function calls ccol_fatal_err(), which aborts with SIGABRT.
 * It does this instead of a risk of a use-after-free or a double free. The
 * rule covers a plain double destroy one after the other, and a concurrent one
 * that overlaps in time. CCOL_EVENT_LOOP_INVALID (0) is the one exception. It
 * stays a silent no-op, which matches the idiom of ccol_event_loop_destroy:
 * a destroy sets the handle to the invalid value.
 *
 * A call on loop from inside a callback is also a fatal error, for the same
 * reason. That is true when the callback runs on one of the threads of loop.
 * That thread is the poller thread. For num_reactor_threads > 1 it is a
 * dispatch worker. This call would
 * need to join that thread, through an internal ccol_event_loop_shutdown. It
 * would also free every registration and the loop struct itself while a
 * callback still runs on that same thread. That is a use-after-free, and not
 * only a deadlock. Move the destroy to another thread, or do it after the
 * callback returns.
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
 * You can call this on an *lp that is already CCOL_EVENT_LOOP_INVALID. It then
 * does nothing. A call on a stale handle that is not CCOL_EVENT_LOOP_INVALID,
 * and that another path already destroyed, is the same fatal misuse that
 * __ccol_event_loop_destroy documents.
 */
static inline __attribute__((always_inline)) void ___ccol_event_loop_destroy(
    ccol_event_loop *lp) {
  if (lp && *lp) {
    __ccol_event_loop_destroy(*lp);
    *lp = CCOL_EVENT_LOOP_INVALID;
  }
}

/**
 * @brief Destroy a ccol_event_loop and set the handle to
 * CCOL_EVENT_LOOP_INVALID
 *
 * This macro blocks until every resolved use of this handle that is in flight
 * finishes. Do not call it at the same time as another call on the same
 * handle. See the doc comment of __ccol_event_loop_destroy for what happens
 * then. It is a fatal error, and not a silent race.
 *
 * @param loop ccol_event_loop to destroy. The macro sets it to
 * CCOL_EVENT_LOOP_INVALID after the destroy.
 *
 * @note You can call this with a CCOL_EVENT_LOOP_INVALID handle
 * @warning Do not call this on loop from inside a callback that dispatches
 * now on one of the threads of loop. That is a self-destroy hazard. It is the
 * destroy-side twin of the self-join hazard of ccol_event_loop_shutdown. It is
 * a fatal error that aborts with SIGABRT. It is not a silent no-op, and it is
 * not a deadlock. Move the destroy to another thread, or do it after the
 * callback returns.
 *
 * @note The macro evaluates loop exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define ccol_event_loop_destroy(loop) \
  _ccol_event_loop_destroy_impl(      \
      loop, _ccol_uniq(__ccol_event_loop_destroy_slot, __COUNTER__))

/* Internal. The body of ccol_event_loop_destroy. slot is a name from
 * _ccol_uniq(), so the macro nests inside the argument of another destroy
 * macro and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_event_loop_destroy_impl(loop, slot) \
  do {                                            \
    __typeof__(loop) *slot = &(loop);             \
    __ccol_event_loop_destroy(*slot);             \
    *slot = CCOL_EVENT_LOOP_INVALID;              \
  } while (0)

/**
 * @brief Declare a ccol_event_loop variable that has no value yet
 *
 * A ccol_event_loop_construct call, or a ccol_event_loop_create call, must
 * come after this macro.
 */
#define ccol_event_loop_declare(name) ccol_event_loop name

/**
 * @brief Declare a variable that the program destroys at the end of the scope
 */
#define ccol_event_loop_declare_scoped(name)                          \
  ccol_event_loop name _ccol_destructor(___ccol_event_loop_destroy) = \
      CCOL_EVENT_LOOP_INVALID

/**
 * @brief Declare and initialize in one step. On failure, call ccol_fatal_err.
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
 *                            must be from 1 to INT_MAX. It must also be small
 *                            enough that max_events_per_wait *
 *                            sizeof(struct epoll_event) does not overflow
 *                            size_t.
 * @param num_lock_stripes   Number of lock stripes for the registry of fds
 *                           and entries. The value must be >= 1. A value of 1
 *                           means one registry lock for the whole loop.
 * @param num_reactor_threads Number of background reactor threads. The value
 *                           must be >= 1. A value of 1 means one thread that
 *                           polls and dispatches in line.
 */
#define ccol_event_loop_construct(name, max_events_per_wait, num_lock_stripes, \
                                  num_reactor_threads)                         \
  ccol_event_loop name = CCOL_EVENT_LOOP_INVALID;                              \
  do {                                                                         \
    char *_evl_err = NULL;                                                     \
    (name) = ccol_event_loop_create((max_events_per_wait), (num_lock_stripes), \
                                    (num_reactor_threads), &_evl_err);         \
    if (!(name)) {                                                             \
      ccol_fatal_err("ccol_event_loop_construct('%s'): %s", #name,             \
                     _evl_err ? _evl_err : "unknown error");                   \
    }                                                                          \
  } while (0)

/**
 * @brief Declare, initialize, and destroy at the end of the scope. On
 *        failure, call ccol_fatal_err.
 *
 * @param name               Name of the variable for the ccol_event_loop
 *                            handle
 * @param max_events_per_wait Size of the epoll_wait batch buffer. The value
 *                            must be from 1 to INT_MAX. It must also be small
 *                            enough that max_events_per_wait *
 *                            sizeof(struct epoll_event) does not overflow
 *                            size_t.
 * @param num_lock_stripes   Number of lock stripes for the registry of fds
 *                           and entries. The value must be >= 1. A value of 1
 *                           means one registry lock for the whole loop.
 * @param num_reactor_threads Number of background reactor threads. The value
 *                           must be >= 1. A value of 1 means one thread that
 *                           polls and dispatches in line.
 */
#define ccol_event_loop_construct_scoped(                                      \
    name, max_events_per_wait, num_lock_stripes, num_reactor_threads)          \
  ccol_event_loop name _ccol_destructor(___ccol_event_loop_destroy) =          \
      CCOL_EVENT_LOOP_INVALID;                                                 \
  do {                                                                         \
    char *_evl_err = NULL;                                                     \
    (name) = ccol_event_loop_create((max_events_per_wait), (num_lock_stripes), \
                                    (num_reactor_threads), &_evl_err);         \
    if (!(name)) {                                                             \
      ccol_fatal_err("ccol_event_loop_construct_scoped('%s'): %s", #name,      \
                     _evl_err ? _evl_err : "unknown error");                   \
    }                                                                          \
  } while (0)

/* ========================================================================== */
/*                         UNIT TEST INTERNALS                                */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Make the next allocation of an internal ccol_event_reg handle slot
 *        report a failure. This is for tests.
 *
 * This function arms a one-shot flag for the whole process, and the flag
 * disarms itself. The next time that ccol_event_loop_add makes a public
 * ccol_event_reg handle for a new registration, that step reports a failure.
 * The result is the same as a failed growth allocation of the slot table. The
 * allocator of the target loop does not matter. ccol_event_loop_add then
 * returns CCOL_EVENT_REG_INVALID and puts nothing into the registry.
 *
 * This lets a test reach that failure mode every time. Fault injection is
 * otherwise not practical here. The loop allocates the slot table to a
 * minimum capacity above zero when it is created. That table also shares its
 * allocator with every other allocation of this loop.
 *
 * @note The flag does nothing after the next ccol_event_loop_add call consumes
 *       it. Call this function again to arm a second one.
 */
void ccol_event_loop_test_force_next_reg_slot_acquire_failure(void);

/**
 * @brief Read the registration count of the whole loop at the exact moment of
 *        the most recent forced slot-acquire failure. This is for tests.
 *
 * This lets a test prove the order inside ccol_event_loop_add, directly and
 * every time. That order is: the function takes the handle slot of reg BEFORE
 * it puts reg into the registry of fds and queues, and not after. Without this
 * value, a test can only see the end state, and both orders give the same end
 * state. In both orders, ccol_event_loop_add returns CCOL_EVENT_REG_INVALID
 * and ccol_event_loop_reg_count() is 0 again, because the rollback of a
 * failure after the wiring also restores the count. A snapshot of 0 proves
 * that the loop did not count the registration of this call yet. That is, the
 * loop did not wire it yet at the moment of the forced failure.
 *
 * @return The snapshot of the most recent forced slot-acquire failure. See
 * ccol_event_loop_test_force_next_reg_slot_acquire_failure. The function gives
 * 0 if no such failure happened yet in this process.
 */
size_t ccol_event_loop_test_last_forced_slot_acquire_failure_reg_count(void);

/**
 * @brief Install a function that ccol_event_loop_add calls after the new
 *        registration is live and before the call returns. This is for tests.
 *
 * The hook runs on the thread that calls ccol_event_loop_add, after the
 * registration is wired into the loop and can be dispatched, and before the
 * handle goes back to the caller. A hook that waits for a callback of the
 * registration therefore makes that callback run before ccol_event_loop_add
 * returns, every time. Pass NULL to remove the hook.
 *
 * @param hook Function to call, or NULL
 */
void ccol_event_loop_test_set_add_before_return_hook(void (*hook)(void));

/**
 * @brief Show how many dispatch jobs sit in the queue of the dispatch_pool of
 *        loop, or run on it now. This is for tests.
 *
 * The function gives 0 for a NULL loop. It also gives 0 for a loop that a call
 * built with num_reactor_threads == 1, because that configuration has no
 * dispatch_pool. This is the direct regression check for the EPOLLONESHOT
 * re-arm mechanism. Take an fd selectable or a queue selectable that is ready
 * all the time. It must not make this count grow without a bound while the
 * workers of dispatch_pool still catch up.
 *
 * @param loop  ccol_event_loop to ask
 * @return      Number of dispatch jobs that wait or that run now
 */
size_t ccol_event_loop_dispatch_pool_pending_count_for_tests(
    ccol_event_loop loop);

/**
 * @brief Show how many epoll_wait calls the poller thread of loop finished up
 *        to now. This is for tests.
 *
 * This lets a test find a busy spin in the reactor directly. In a busy spin,
 * this counter runs ahead by a large amount inside a short, bounded window. A
 * test therefore does not need a measurement of the wall clock or of CPU use,
 * which are both flaky. Read this value two times across a bounded interval
 * and compare the difference.
 *
 * @param loop  ccol_event_loop to ask
 * @return      Total count of finished epoll_wait calls. The function gives 0
 * for a loop handle that is not valid or that is stale.
 */
uint64_t ccol_event_loop_poller_iterations_for_tests(ccol_event_loop loop);

/**
 * @brief Make the next ccol_cond_var_timedwait call report an unexpected
 *        error, which is an error other than ETIMEDOUT. The call sits in the
 *        wait path of ccol_select_timed that has no fd selectables. This is
 *        for tests.
 *
 * This function arms a one-shot flag, and the flag disarms itself. The wait of
 * ccol_select_timed that uses only a condition variable calls
 * ccol_cond_var_timedwait. That path is reachable only with a deadline and
 * with no fd selectable in the call. The next such call reports EINVAL and
 * does not wait.
 *
 * This lets a test reach the path where ccol_cond_var_timedwait itself fails,
 * which gives ccol_unexpected_failure. No correct call from the deadline
 * computation of this library itself can reach that path.
 *
 * @note The flag does nothing after the next such call consumes it. Call this
 *       function again to arm a second one.
 */
void ccol_select_test_force_next_condvar_wait_error(void);

/**
 * @brief The same as ccol_select_test_force_next_condvar_wait_error, but it
 *        also marks the internal wait as satisfied at the same instant. This
 *        is for tests.
 *
 * This function arms the same one-shot forced EINVAL as
 * ccol_select_test_force_next_condvar_wait_error. It also sets the internal
 * "ready" flag to true, inside the same critical section that reports the
 * forced error. This imitates a producer whose notify finishes correctly one
 * instant before the unrelated, forced error appears.
 *
 * That order of events is truly possible in production. A real
 * ccol_cond_var_timedwait call always takes its mutex again before it returns,
 * on success and on failure. But a test cannot reach that order in another
 * way every time. This hook replaces the real wait call, so it does not unlock
 * the mutex for a producer thread to race into.
 *
 * This lets a test check one property every time. A wakeup that races an
 * unrelated, spurious failure of ccol_cond_var_timedwait still counts as a
 * successful wakeup. ccol_select_timed then scans again and succeeds in the
 * end. The library does not drop that wakeup and does not report
 * ccol_unexpected_failure.
 *
 * @note The flag does nothing after the next such call consumes it. Call this
 *       function again to arm a second one.
 */
void ccol_select_test_force_next_condvar_wait_error_racing_ready(void);

/**
 * @brief For tests only: delay the next timed-out deregistration of
 *        ccol_select_timed
 *
 * This function arms a one-shot delay, and the delay disarms itself. The delay
 * sits between two points. At the first point, the wait of ccol_select_timed
 * that uses only a condition variable reports that its deadline ended. At the
 * second point, that call unlinks its waiter nodes from the queues that it
 * watched. A notify from a producer that lands in that window goes to a waiter
 * that already stopped its wait. The waiter that leaves must therefore pass
 * the wake on. In production that window is a few instructions, which is much
 * too narrow for a test to aim at.
 *
 * @param us Delay in microseconds. A value of 0 disarms a delay that did not
 * run yet.
 *
 * @note The next such deregistration consumes the delay. Call this function
 * again to arm another one.
 */
void ccol_select_test_delay_next_timed_out_deregister_us(uint32_t us);

/**
 * @brief For tests only: how many times a thread entered the delay that
 *        ccol_select_test_delay_next_timed_out_deregister_us armed
 *
 * A test polls this value in a bounded loop. It then knows that the delayed
 * waiter truly reached that window. Only after that does it produce the
 * message whose notify must land inside the window. The test therefore does
 * not guess with a sleep.
 */
uint64_t ccol_select_test_timed_out_deregister_delay_count(void);

/**
 * @brief Make the next ccol_cond_var_timedwait call in the wait loop of
 * ccol_circq_timed_send_zc report an unexpected error, which is an error other
 * than ETIMEDOUT. This is for tests.
 *
 * This works like ccol_select_test_force_next_condvar_wait_error, for
 * ccol_circq_timed_send_zc instead. The next wait of that function reports
 * EINVAL and does not wait. The flag then disarms itself.
 *
 * @note The flag does nothing after the next such call consumes it. Call this
 *       function again to arm a second one.
 * @see ccol_circq_test_force_next_send_condvar_wait_error_racing_ready
 */
void ccol_circq_test_force_next_send_condvar_wait_error(void);

/**
 * @brief For tests only: make the window inside _notify_waiter() wider. The
 *        window starts where a thread already holds the mutex of a queue. It
 *        ends where this function tries to lock wait_mtx. The new width is
 *        any duration that the test chooses, in microseconds. A value of 0
 *        turns the widening off, which is the default.
 *
 * This lets a test land a concurrent fork() call inside that exact window,
 * every time. The test then reproduces the lock-order hazard between the mutex
 * of the queue and wait_mtx. The merged atfork handler of ccol_event_loop and
 * the queue registry exists to prevent that hazard. In production the window
 * is normally only a few instructions, so a test cannot aim at it by luck.
 *
 * @warning While the widening is armed, it changes every _notify_waiter() call
 * in the whole process. There is no scope for one queue. A test must disarm it
 * with a value of 0 when it finishes. Without that, every later
 * ccol_circq_send_zc call, ccol_circq_recv_zc call, ccol_dynmq_send_zc call,
 * and every other such call anywhere in the same process pays this delay.
 */
void _notify_waiter_test_set_delay_us(int us);

/**
 * @brief Make the next internal unpin of a ccol_event_reg sleep for ms
 *        milliseconds while it still holds its resolve pin. This is for tests.
 *
 * This function arms a one-shot flag for the whole process, and the flag
 * disarms itself. Several public calls resolve a ccol_event_reg and then unpin
 * it: ccol_event_loop_modify, ccol_event_loop_pause, ccol_event_loop_resume,
 * ccol_event_loop_remove, and ccol_event_loop_reg_generation. The next such
 * call sleeps for ms milliseconds when it reaches the point where it frees its
 * resolve pin.
 *
 * This lets a test hold a resolve pin open long enough, every time. Another
 * thread can then run ccol_event_loop_remove on the same registration. That
 * thread marks the registration as removed and defers it first. The test
 * therefore reaches one exact case. In that case the delivery of an
 * on_removed notification depends on the bounded reclaim retry of the
 * reactor. It does not depend on the wakeup ping of one call.
 *
 * @param ms  Milliseconds to sleep. A value of 0 turns the delay off, which is
 * the default.
 * @note The flag does nothing after the next unpin consumes it. Call this
 *       function again to arm a second one.
 */
void ccol_event_loop_test_delay_next_reg_resolve_unpin_ms(uint32_t ms);

/**
 * @brief For tests only: lock the internal mutex of cq directly, and go around
 *        every function of the public API.
 *
 * This lets a test keep the mutex of cq locked for a window of any length that
 * the test controls exactly. The window is long enough to land a fork() call
 * inside it every time. A real critical section in production is normally only
 * a few instructions long, so a test cannot aim at it by luck. The regression
 * test for the fork safety of the queue mutex uses this function.
 *
 * @warning A later ccol_circq_test_unlock_mutex_for_tests call on the same cq,
 * from the same thread, must always follow this call. Nothing else in this
 * file expects a thread to still hold the mutex of cq after this call returns.
 *
 * @see ccol_circq_test_unlock_mutex_for_tests
 */
void ccol_circq_test_lock_mutex_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: unlock the internal mutex of cq. See
 *        ccol_circq_test_lock_mutex_for_tests.
 */
void ccol_circq_test_unlock_mutex_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: lock the write side of the reg_slot_rwlock of loop
 *        directly, and go around every function of the public API.
 *
 * This lets a test keep the write side of this rwlock locked for a window of
 * any length that the test controls exactly. The thread that holds the lock
 * must be a thread OTHER than the thread that calls fork().
 * ccol_circq_test_lock_mutex_for_tests exists for the same reason. This
 * function covers the reg-slot table of a ccol_event_loop instead of a
 * ccol_circular_queue.
 *
 * @return An opaque pointer to the resolved loop. You MUST give this pointer
 *         to the matching ccol_event_loop_test_wrunlock_reg_slot_for_tests
 *         call. The function gives NULL if loop was not valid. It then locked
 *         nothing, and the matching unlock call with NULL does nothing and is
 *         safe. The value is deliberately not loop itself. The unlock side
 *         must use this exact resolved pointer, and it must not resolve loop
 *         again. A second resolve from another thread can deadlock against a
 *         concurrent fork(). That fork() already holds the mutex of
 *         ccol_event_loop_slot_table while it waits for this exact write lock
 *         to become free. That deadlock is real and reachable, and not a
 *         theoretical concern.
 *
 * @warning A later ccol_event_loop_test_wrunlock_reg_slot_for_tests call must
 * always follow this call, with the return value of this call. Nothing else in
 * this file expects reg_slot_rwlock to still hold a write lock after that call
 * returns.
 *
 * @see ccol_event_loop_test_wrunlock_reg_slot_for_tests
 */
void *ccol_event_loop_test_wrlock_reg_slot_for_tests(ccol_event_loop loop);

/**
 * @brief For tests only: unlock the write side of reg_slot_rwlock that an
 *        earlier ccol_event_loop_test_wrlock_reg_slot_for_tests call locked.
 *
 * @param resolved_loop The exact return value of the matching
 *        ccol_event_loop_test_wrlock_reg_slot_for_tests call. That value is
 *        not NULL. A NULL value is safe and does nothing. That call also
 *        gives NULL for a loop that is not valid.
 *
 * @see ccol_event_loop_test_wrlock_reg_slot_for_tests
 */
void ccol_event_loop_test_wrunlock_reg_slot_for_tests(void *resolved_loop);

/**
 * @brief For tests only: lock the write side of the rwlock of
 *        ccol_event_loop_slot_table directly, and go around every function of
 *        the public API.
 *
 * This lets a test keep the write side of this rwlock locked for a window of
 * any length that the test controls exactly. The thread that holds the lock
 * must be a thread OTHER than the thread that calls fork(). This function
 * covers the loop table of the whole process. That table is not the same as
 * the reg_slot_rwlock of one loop, which
 * ccol_event_loop_test_wrlock_reg_slot_for_tests covers. There is no handle of
 * one instance to resolve here. This function therefore takes no argument, and
 * the matching unlock call needs none of the AB-BA precautions of that other
 * pair.
 *
 * @warning A later ccol_event_loop_test_wrunlock_slot_table_for_tests call
 * must always follow this call. Nothing else in this file expects
 * ccol_event_loop_slot_table.rwlock to still hold a write lock after that call
 * returns.
 *
 * @see ccol_event_loop_test_wrunlock_slot_table_for_tests
 */
void ccol_event_loop_test_wrlock_slot_table_for_tests(void);

/**
 * @brief For tests only: unlock the write side of the rwlock of
 *        ccol_event_loop_slot_table. See
 *        ccol_event_loop_test_wrlock_slot_table_for_tests.
 */
void ccol_event_loop_test_wrunlock_slot_table_for_tests(void);

/**
 * @brief For tests only: report whether a ccol_select() caller is truly linked
 *        into the read-waiter list of cq right now.
 *
 * The function locks, checks, and unlocks. It is self-contained. Some tests
 * need a select-waiter thread to reach its own Phase 1 ccol_mutex_lock and
 * link step. Such a test polls this value in a bounded loop. It therefore does
 * not guess that a fixed sleep after pthread_create() gives the OS enough time
 * to schedule the new thread that far. The return of pthread_create() gives no
 * such guarantee.
 */
bool ccol_circq_test_has_sel_read_waiter_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: how many ccol_select() waiter nodes are linked into
 *        the read-waiter list of cq right now.
 *
 * This function counts, where ccol_circq_test_has_sel_read_waiter_for_tests
 * only answers yes or no. Some tests need an exact number of ccol_select()
 * callers to reach their own link step, and not only one or more. The order in
 * which two waiters link decides which of them the next notify of a producer
 * goes to. A test that depends on that order polls this value until each
 * waiter is in place, and only then starts the next waiter.
 */
size_t ccol_circq_test_sel_read_waiter_count_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: how many receivers park now in the read condition
 *        variable of cq.
 *
 * A send signals that condition variable only when this count is not zero.
 * Some tests must prove that a send truly wakes a blocked receiver, and that
 * the receiver did not leave on its own timeout. Such a test polls this value
 * in a bounded loop until the receiver registers itself, and only then it
 * sends. A wait site that does not register never lets that poll succeed. This
 * is what stops such a test from passing while it examines nothing.
 */
size_t ccol_circq_waiting_readers_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: how many senders park now in the write condition
 *        variable of cq. This is the send-side twin of
 *        ccol_circq_waiting_readers_for_tests.
 */
size_t ccol_circq_waiting_writers_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: how many receivers park now in the read condition
 *        variable of dq. This is ccol_circq_waiting_readers_for_tests for a
 *        dynamic queue.
 */
size_t ccol_dynmq_waiting_readers_for_tests(ccol_dynamic_queue *dq);

/**
 * @brief The same as ccol_circq_test_force_next_send_condvar_wait_error, but
 * it also frees the slot of one queued message at the same instant. This is
 * for tests.
 *
 * This function arms the same one-shot forced EINVAL. It also does the exact
 * bookkeeping of a real receive by a concurrent consumer: it frees one slot
 * and signals write_cond. It does that work under the same mutex that reports
 * the forced error. This imitates a consumer whose wakeup finishes correctly
 * one instant before the unrelated, forced error appears. This lets a test
 * check that the send still uses a slot that became free in this way. The
 * library does not drop that slot and does not report
 * ccol_unexpected_failure.
 *
 * @note The flag does nothing after the next such call consumes it. Call this
 *       function again to arm a second one.
 */
void ccol_circq_test_force_next_send_condvar_wait_error_racing_ready(void);

/**
 * @brief Get the message that the most recent race freed. The race comes from
 *        ccol_circq_test_force_next_send_condvar_wait_error_racing_ready. This
 *        is for tests.
 *
 * The receive of the imitated concurrent consumer takes a message out of the
 * queue. See ccol_circq_test_force_next_send_condvar_wait_error_racing_ready.
 * That receive has nowhere else to put the message. This function gives the
 * message exactly one time, and it gives {NULL, 0} after that. A test can then
 * free the data pointer of the message. This matches the ownership that a real
 * caller of ccol_circq_recv_zc or ccol_circq_try_recv_zc already has.
 *
 * @return The freed message, or {NULL, 0} if no message waits
 */
c_message_t ccol_circq_test_take_race_freed_msg(void);

/**
 * @brief Make the next ccol_cond_var_timedwait call in the wait loop of
 * ccol_circq_timed_recv_zc report an unexpected error, which is an error other
 * than ETIMEDOUT. This is for tests.
 *
 * This works like ccol_select_test_force_next_condvar_wait_error, for
 * ccol_circq_timed_recv_zc instead.
 *
 * @note The flag does nothing after the next such call consumes it. Call this
 *       function again to arm a second one.
 * @see ccol_circq_test_force_next_recv_condvar_wait_error_racing_ready
 */
void ccol_circq_test_force_next_recv_condvar_wait_error(void);

/**
 * @brief The same as ccol_circq_test_force_next_recv_condvar_wait_error, but
 *        it also puts a sentinel message into the queue at the same instant.
 *        This is for tests.
 *
 * This function arms the same one-shot forced EINVAL. It also puts a
 * {NULL, 0} sentinel message into the queue. It uses the same internal path as
 * the send of a real concurrent producer. It does that work under the same
 * mutex that reports the forced error. This imitates a producer whose wakeup
 * finishes correctly one instant before the unrelated, forced error appears.
 * This lets a test check that the library still receives a message that
 * arrived in this way. The library does not drop that message and does not
 * report ccol_unexpected_failure.
 *
 * @note The flag does nothing after the next such call consumes it. Call this
 *       function again to arm a second one.
 */
void ccol_circq_test_force_next_recv_condvar_wait_error_racing_ready(void);

/**
 * @brief Make the next ccol_cond_var_timedwait call in the wait loop of
 * ccol_dynmq_timed_recv_zc report an unexpected error, which is an error other
 * than ETIMEDOUT. This is for tests.
 *
 * This works like ccol_select_test_force_next_condvar_wait_error, for
 * ccol_dynmq_timed_recv_zc instead.
 *
 * @note The flag does nothing after the next such call consumes it. Call this
 *       function again to arm a second one.
 * @see ccol_dynmq_test_force_next_recv_condvar_wait_error_racing_ready
 */
void ccol_dynmq_test_force_next_recv_condvar_wait_error(void);

/**
 * @brief The same as ccol_dynmq_test_force_next_recv_condvar_wait_error, but
 *        it also puts a sentinel message into the queue at the same instant.
 *        This is for tests.
 *
 * This function arms the same one-shot forced EINVAL. It also puts a
 * {NULL, 0} sentinel message into the queue. It uses the same internal path as
 * the send of a real concurrent producer. It does that work under the same
 * mutex that reports the forced error. This imitates a producer whose wakeup
 * finishes correctly one instant before the unrelated, forced error appears.
 * This lets a test check that the library still receives a message that
 * arrived in this way. The library does not drop that message and does not
 * report ccol_unexpected_failure.
 *
 * @note The flag does nothing after the next such call consumes it. Call this
 *       function again to arm a second one.
 */
void ccol_dynmq_test_force_next_recv_condvar_wait_error_racing_ready(void);

/**
 * @brief Delay the next cascade notification of a queue by us microseconds.
 *        This is for tests.
 *
 * This function arms a one-shot flag for the whole process, and the flag
 * disarms itself. The next cascade step after a dispatch, for a queue
 * registration or a ccol_channel registration, sleeps for us microseconds. It
 * sleeps after it starts to look at the queue and before it touches the mutex
 * of the queue. This makes one window wide enough for a test to land inside it
 * every time. In that window, the library must hold off a concurrent
 * ccol_event_loop_remove call and the destroy of the queue that comes
 * immediately after it.
 *
 * @param us  Microseconds to sleep. A value of 0 disarms the delay.
 */
void ccol_event_loop_test_delay_next_queue_cascade_us(uint32_t us);

/**
 * @brief Read how many cascade steps of a queue started, and how many stopped
 *        to touch the queue. This is for tests.
 *
 * Both counters cover the whole process, and both only go up. A cascade step
 * adds one to the first counter when it starts to look at the queue. It adds
 * one to the second counter when it finishes with the queue. A test can
 * therefore prove one property: a concurrent ccol_event_loop_remove cannot
 * return while a cascade step is still between the two counters. That property
 * is what makes a destroy of the queue safe immediately after that call.
 *
 * @param begun     Optional. It gets the count of the steps that started.
 * @param finished  Optional. It gets the count of the steps that finished.
 */
void ccol_event_loop_test_queue_cascade_counts(uint64_t *begun,
                                               uint64_t *finished);

/**
 * @brief Read how many wakes the queue notify path delivered through an
 *        eventfd. This is for tests.
 *
 * The counter covers the whole process and only goes up. It counts each
 * write(2) that a send, a receive, a state change or a cascade step makes to
 * the eventfd of a ccol_select() waiter in epoll mode or of a ccol_event_loop
 * queue registration. It does not count the internal eventfds of a
 * ccol_event_loop itself. A test samples it before and after a burst of sends
 * and asserts that a listener with a wake already pending costs no further
 * write.
 *
 * @return Number of eventfd wakes so far
 */
uint64_t ccol_select_test_eventfd_wake_count(void);
#endif

#pragma GCC visibility pop
