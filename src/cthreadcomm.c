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

#include <cthreadcomm.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef RUNNING_UNIT_TESTS
#include <assert.h>
#endif

/* Adds duration to target, normalising tv_nsec into [0, 1e9) to keep the
 * struct in a valid state for pthread_cond_timedwait. Both inputs are
 * normalised independently so the function is safe even when either
 * carries an already-overflowed tv_nsec. */
void add_duration_to_timespec(struct timespec *target,
                              struct timespec *duration) {
  static const long int max_nsecs = 1000000000;

  if (target->tv_nsec >= max_nsecs) {
    target->tv_sec += target->tv_nsec / max_nsecs;
    target->tv_nsec = target->tv_nsec % max_nsecs;
  }

  struct timespec dur =
      *duration;  // local copy — avoid mutating caller's struct
  if (dur.tv_nsec >= max_nsecs) {
    dur.tv_sec += dur.tv_nsec / max_nsecs;
    dur.tv_nsec = dur.tv_nsec % max_nsecs;
  }

  target->tv_sec += dur.tv_sec;

  long int gap = max_nsecs - target->tv_nsec;

  if (gap > dur.tv_nsec) {
    target->tv_nsec += dur.tv_nsec;
  } else {
    ++target->tv_sec;
    target->tv_nsec = dur.tv_nsec - gap;
  }
}

/* A waiter node registered by a thread blocked in ccol_select. Each node lives
 * on the heap for the full duration of that call (heap-allocated to avoid stack
 * overflow for large n).  The owning queue's mutex must be held whenever the
 * waiter list is read or modified, which guarantees that these nodes remain
 * valid during any traversal by a producer. */
typedef struct ccol_sel_waiter {
  pthread_mutex_t *sel_mtx;
  pthread_cond_t *sel_cond;
  bool *ready;
  int efd; /* eventfd for epoll mode; -1 in condvar-only mode */
  struct ccol_sel_waiter *prev;
  struct ccol_sel_waiter *next;
} ccol_sel_waiter;

/* Wakes the single head waiter on a queue.  Used for message/slot events where
 * exactly one resource became available; waking more than one waiter would
 * cause a thundering herd.  The cascade mechanism inside ccol_select's Phase 1
 * propagates the wake further when additional resources remain after the first
 * woken thread claims its resource.  Must be called while the queue's own mutex
 * is held so that the waiter node (on a foreign thread's stack) remains valid. */
static void notify_one_sel_waiter(ccol_sel_waiter *head) {
  if (!head) return;
  pthread_mutex_lock(head->sel_mtx);
  *head->ready = true;
  pthread_mutex_unlock(head->sel_mtx);
  pthread_cond_signal(head->sel_cond);
  if (head->efd >= 0) {
    uint64_t one = 1;
    (void)write(head->efd, &one, sizeof(one));
  }
}

/* Wakes every thread currently blocked in ccol_select on this queue.  Used
 * exclusively for state-change events (disable_sending, enable_sending) where
 * every blocked thread must re-evaluate regardless of resource availability.
 * Must be called while the queue's own mutex is held. */
static void notify_all_sel_waiters(ccol_sel_waiter *head) {
  for (ccol_sel_waiter *w = head; w != NULL; w = w->next) {
    pthread_mutex_lock(w->sel_mtx);
    *w->ready = true;
    pthread_mutex_unlock(w->sel_mtx);
    pthread_cond_signal(w->sel_cond);
    if (w->efd >= 0) {
      uint64_t one = 1;
      (void)write(w->efd, &one, sizeof(one));
    }
  }
}

struct circular_queue {
  mutex_t mutex;
  cond_var_t read_cond;
  cond_var_t write_cond;

  size_t read_index;
  size_t write_index;
  size_t max_size;
  size_t msg_count;

  ccol_memmgmt_procs_t *m_procs;

  c_message_t *msg_array;
  bool writing_disabled;

  ccol_sel_waiter *sel_read_waiters_head;
  ccol_sel_waiter *sel_write_waiters_head;
};

/* Validates circular_queue creation arguments: max_size must be positive and
 * within max_elem_count, and the custom allocator (if any) must be well-formed.
 */
bool verify_circular_queue_create_inputs(size_t max_size,
                                         ccol_memmgmt_procs_t *mmgmt_procs,
                                         char **err_str) {
  if (max_size == 0) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("max_size should be positive");
    }
    return false;
  }

  if (max_size > max_elem_count) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("max_size can not exceed max_elem_count");
    }
    return false;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return false;
  }

  return true;
}

/* Allocates and initialises a bounded circular queue with a fixed-size message
 * array. The mutex and both condition variables are initialised here. The queue
 * starts with writing enabled. */
circular_queue *circular_queue_create_with_mprocs(
    size_t max_size, ccol_memmgmt_procs_t *mmgmt_procs, char **err_str) {
  if (!verify_circular_queue_create_inputs(max_size, mmgmt_procs, err_str)) {
    return NULL;
  }

  circular_queue *cq =
      (circular_queue *)_mem_alloc(mmgmt_procs, sizeof(circular_queue));
  if (!cq) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("Failed to allocate memory for circular_queue");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(cq, mmgmt_procs, err_str)) {
    _mem_free(mmgmt_procs, cq);
    return NULL;
  }

  cq->msg_array =
      (c_message_t *)_mem_alloc(mmgmt_procs, max_size * sizeof(c_message_t));
  if (!cq->msg_array) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("Failed to allocate memory for cq msg_array");
    }
    _mem_free(mmgmt_procs, cq->m_procs);
    _mem_free(mmgmt_procs, cq);
    return NULL;
  }

  mutex_init(cq->mutex);
  cond_var_init(cq->read_cond);
  cond_var_init(cq->write_cond);
  cq->read_index = 0;
  cq->write_index = 0;
  cq->max_size = max_size;
  cq->msg_count = 0;
  cq->writing_disabled = false;
  cq->sel_read_waiters_head = NULL;
  cq->sel_write_waiters_head = NULL;

  if (err_str) {
    *err_str = NULL;
  }

  return cq;
}

/* Destroys the circular queue. Asserts if any messages remain unconsumed,
 * because their data pointers would be leaked – that is a bug on the caller's
 * side and must be made visible. */
void __circular_queue_destroy(circular_queue *cq) {
  if (cq) {
    if (circq_msg_count(cq) > 0) {
      // The data pointers of the messages that
      // haven't been consumed are going to be
      // leaked. That's a bug on the caller side.
      // Let's make it noticed.
      ccol_assert(false);
    }

    if (cq->msg_array) {
      _mem_free(cq->m_procs, cq->msg_array);
      cq->msg_array = NULL;
    }

    mutex_destroy(cq->mutex);
    cond_var_destroy(cq->read_cond);
    cond_var_destroy(cq->write_cond);

    if (cq->m_procs) {
      ccol_free_t free_func = cq->m_procs->free;
      free_func(cq->m_procs);
      free_func(cq);
    } else {
      mem_free(cq);
    }
  }
}

/* Writes msg into the circular array at write_index and advances the index
 * (wrapping to 0 at max_size). Nullifies msg->data to transfer ownership to
 * the receiver (zero-copy contract). Must be called with the mutex held. */
void _sendto_cq(circular_queue *cq, c_message_t *msg) {
  cq->msg_array[cq->write_index].data = msg->data;
  cq->msg_array[cq->write_index++].size = (msg->data == NULL) ? 0 : msg->size;
  msg->data = NULL;  // The sender loses the ownership of the msg pointer.
  if (cq->write_index == cq->max_size) {
    cq->write_index = 0;
  }
  ++cq->msg_count;

  cond_var_signal(cq->read_cond);
  notify_one_sel_waiter(cq->sel_read_waiters_head);
}

/* Validates send arguments: the queue and message must be non-NULL, and a
 * message with a non-NULL data pointer must have a non-zero size (size == 0
 * with data != NULL would be an inconsistent state). */
bool verify_circq_send_zc_params(circular_queue *cq, c_message_t *msg) {
  if (!cq || !msg || (msg->size == 0 && msg->data != NULL)) {
    return false;
  }

  return true;
}

/* Blocking send: waits on write_cond until there is space in the queue, then
 * transfers ownership of msg->data to the queue. Returns ccol_not_permitted
 * immediately if writing has been disabled (checked before and after the wait
 * to handle races with circq_disable_sending). */
ccol_retval_t circq_send_zc(circular_queue *cq, c_message_t *msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  while (cq->msg_count == cq->max_size && !cq->writing_disabled) {
    cond_var_wait(cq->write_cond, cq->mutex);
  }

  if (cq->writing_disabled) {
    mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  _sendto_cq(cq, msg);

  mutex_unlock(cq->mutex);

  return ccol_success;
}

/* Non-blocking send: returns ccol_container_full immediately when the queue is
 * full rather than waiting. Returns ccol_not_permitted if writing is disabled.
 */
ccol_retval_t circq_try_send_zc(circular_queue *cq, c_message_t *msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  // Assuming we won't have space for the new message.
  ccol_retval_t result = ccol_container_full;

  mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  if (cq->msg_count < cq->max_size) {
    // We have space for the new message, proceed.
    _sendto_cq(cq, msg);
    result = ccol_success;
  }

  mutex_unlock(cq->mutex);

  return result;
}

/* Timed send: waits up to timeout_duration for space. The absolute deadline is
 * computed once before the wait loop so repeated spurious wake-ups cannot
 * extend the timeout. Returns ccol_timed_out on expiry. */
ccol_retval_t circq_timed_send_zc(circular_queue *cq, c_message_t *msg,
                                  struct timespec *timeout_duration) {
  if (!verify_circq_send_zc_params(cq, msg) || !timeout_duration) {
    return ccol_invalid_args;
  }

  mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  if (cq->msg_count == cq->max_size) {
    int retval;
    struct timespec abs_time;
    clock_gettime(CLOCK_REALTIME, &abs_time);
    add_duration_to_timespec(&abs_time, timeout_duration);

    while (cq->msg_count == cq->max_size && !cq->writing_disabled) {
      if ((retval = cond_var_timedwait(cq->write_cond, cq->mutex, abs_time))) {
        if (retval != ETIMEDOUT) {
          mutex_unlock(cq->mutex);
          return ccol_unexpected_failure;
        }
        mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
  }

  if (cq->writing_disabled) {
    mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  _sendto_cq(cq, msg);

  mutex_unlock(cq->mutex);

  return ccol_success;
}

/* Reads one message from the circular array at read_index, advances the index
 * (wrapping to 0 at max_size), and signals write_cond so any blocked sender
 * can proceed. Must be called with the mutex held. */
void _recvfrom_cq(circular_queue *cq, c_message_t *target_buf) {
  target_buf->data = cq->msg_array[cq->read_index].data;
  target_buf->size = cq->msg_array[cq->read_index++].size;
  if (cq->read_index == cq->max_size) {
    cq->read_index = 0;
  }

  --cq->msg_count;

  cond_var_signal(cq->write_cond);
  notify_one_sel_waiter(cq->sel_write_waiters_head);
}

/* Validates receive arguments: both the queue and the target buffer must be
 * non-NULL. */
bool verify_recvfrom_cq_zc_params(circular_queue *cq, c_message_t *target_buf) {
  if (!cq || !target_buf) {
    return false;
  }

  return true;
}

/* Blocking receive: waits on read_cond until at least one message is available,
 * then transfers ownership to target_buf. */
ccol_retval_t circq_recv_zc(circular_queue *cq, c_message_t *target_buf) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  mutex_lock(cq->mutex);

  while (cq->msg_count == 0 && !cq->writing_disabled) {
    cond_var_wait(cq->read_cond, cq->mutex);
  }

  if (cq->msg_count == 0) {
    mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  _recvfrom_cq(cq, target_buf);

  mutex_unlock(cq->mutex);

  return ccol_success;
}

/* Non-blocking receive: returns ccol_container_empty immediately when no
 * messages are available. */
ccol_retval_t circq_try_recv_zc(circular_queue *cq, c_message_t *target_buf) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_empty;

  mutex_lock(cq->mutex);

  if (cq->msg_count > 0) {
    result = ccol_success;
    _recvfrom_cq(cq, target_buf);
  }

  mutex_unlock(cq->mutex);

  return result;
}

/* Timed receive: waits up to timeout for a message. Same absolute-deadline
 * strategy as circq_timed_send_zc to prevent timeout drift on spurious wakes.
 */
ccol_retval_t circq_timed_recv_zc(circular_queue *cq, c_message_t *target_buf,
                                  struct timespec *timeout) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf) || !timeout) {
    return ccol_invalid_args;
  }

  mutex_lock(cq->mutex);

  if (cq->msg_count == 0) {
    int retval;
    struct timespec abs_time;
    clock_gettime(CLOCK_REALTIME, &abs_time);
    add_duration_to_timespec(&abs_time, timeout);

    while (cq->msg_count == 0 && !cq->writing_disabled) {
      if ((retval = cond_var_timedwait(cq->read_cond, cq->mutex, abs_time))) {
        if (retval != ETIMEDOUT) {
          mutex_unlock(cq->mutex);
          return ccol_unexpected_failure;
        }
        mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
  }

  if (cq->msg_count == 0) {
    mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  _recvfrom_cq(cq, target_buf);

  mutex_unlock(cq->mutex);

  return ccol_success;
}

/* Sets the writing_disabled flag and broadcasts on write_cond to wake all
 * threads blocked in circq_send_zc so they can observe the disabled state
 * and return ccol_not_permitted. */
ccol_retval_t circq_disable_sending(circular_queue *cq) {
  if (cq) {
    mutex_lock(cq->mutex);
    cq->writing_disabled = true;
    cond_var_broadcast(cq->write_cond);  // Wake waiting senders
    cond_var_broadcast(cq->read_cond);   // Wake blocked receivers so they can
                                         // observe the disabled state
    notify_all_sel_waiters(cq->sel_read_waiters_head);
    notify_all_sel_waiters(cq->sel_write_waiters_head);
    mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

/* Clears the writing_disabled flag and broadcasts on write_cond to wake any
 * threads that were blocked while the queue was disabled. */
ccol_retval_t circq_enable_sending(circular_queue *cq) {
  if (cq) {
    mutex_lock(cq->mutex);
    cq->writing_disabled = false;
    cond_var_broadcast(cq->write_cond);  // Wake waiting senders
    notify_all_sel_waiters(cq->sel_write_waiters_head);
    mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

/* Returns the number of messages currently in the queue. Acquires the mutex
 * to get a consistent snapshot. Returns ccol_invalid_size if cq is NULL. */
size_t circq_msg_count(circular_queue *cq) {
  size_t result = ccol_invalid_size;

  if (cq) {
    mutex_lock(cq->mutex);
    result = cq->msg_count;
    mutex_unlock(cq->mutex);
  }

  return result;
}

// Dynamic queue related section starts here.
typedef struct dllist_node {
  struct dllist_node *prev;
  c_message_t msg;
  struct dllist_node *next;
} dllist_node;

struct dynamic_queue {
  mutex_t mutex;
  cond_var_t read_cond;

  size_t msg_count;

  dllist_node *head;
  dllist_node *tail;

  ccol_memmgmt_procs_t *m_procs;

  bool writing_disabled;

  ccol_sel_waiter *sel_read_waiters_head;
  ccol_sel_waiter *sel_write_waiters_head;
};

/* Allocates a new dllist_node, copies the message metadata into it, nullifies
 * msg->data to transfer ownership, and appends the node to the tail of the
 * queue's doubly-linked list. Must be called with the mutex held. */
ccol_retval_t append_msg_to_dq_tail(dynamic_queue *dq, c_message_t *msg) {
  dllist_node *new_elem =
      (dllist_node *)_mem_alloc(dq->m_procs, sizeof(dllist_node));
  if (!new_elem) {
    return ccol_not_enough_memory;
  }

  new_elem->msg.data = msg->data;
  new_elem->msg.size = (msg->data == NULL) ? 0 : msg->size;
  msg->data = NULL;
  new_elem->next = NULL;

  if (!dq->head) {
#ifdef RUNNING_UNIT_TESTS
    ccol_assert(!dq->tail);
#endif
    new_elem->prev = NULL;
    dq->head = new_elem;
    dq->tail = new_elem;
  } else {
#ifdef RUNNING_UNIT_TESTS
    ccol_assert(dq->tail && !dq->tail->next);
#endif
    new_elem->prev = dq->tail;
    dq->tail->next = new_elem;
    dq->tail = new_elem;
  }

  return ccol_success;
}

/* Removes and returns the head node's message. When the list becomes empty
 * both head and tail are set to NULL to keep the invariant consistent. The
 * node struct is freed after its message is copied out. Must be called with
 * the mutex held. */
ccol_retval_t remove_msg_from_dq_head(dynamic_queue *dq,
                                      c_message_t *target_buf) {
  if (!dq->head) {
#ifdef RUNNING_UNIT_TESTS
    ccol_assert(!dq->tail);
#endif
    return ccol_container_empty;
  }

#ifdef RUNNING_UNIT_TESTS
  ccol_assert(!dq->head->prev);
  ccol_assert(dq->tail && !dq->tail->next);
#endif

  dllist_node *node_to_be_freed = dq->head;

  target_buf->data = dq->head->msg.data;
  target_buf->size = dq->head->msg.size;

  dq->head = dq->head->next;
  if (dq->head) {
    dq->head->prev = NULL;
  } else {
    // The head just became NULL, let's not forget about the tail
    dq->tail = NULL;
  }

  _mem_free(dq->m_procs, node_to_be_freed);
  return ccol_success;
}

/* Frees all dllist_node structs in the dynamic queue. Does not free the data
 * pointers stored in each message – those should have already been consumed
 * (the destroy function asserts non-zero msg_count to catch leaks). */
void destroy_dq_dllist(dynamic_queue *dq) {
  dllist_node *node_to_be_freed = NULL;
  while (dq->head) {
    node_to_be_freed = dq->head;
    dq->head = dq->head->next;
    _mem_free(dq->m_procs, node_to_be_freed);
  }
  dq->tail = NULL;
}

/* Allocates and initialises an unbounded dynamic queue backed by a doubly-
 * linked list. Unlike circular_queue, it never blocks on send (the list grows
 * with each message). Only a read_cond is needed; no write_cond is required. */
dynamic_queue *dynamic_queue_create_with_mprocs(
    ccol_memmgmt_procs_t *mmgmt_procs, char **err_str) {
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  dynamic_queue *dq =
      (dynamic_queue *)_mem_alloc(mmgmt_procs, sizeof(dynamic_queue));
  if (!dq) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("Failed to allocate memory for dynamic_queue");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(dq, mmgmt_procs, err_str)) {
    _mem_free(mmgmt_procs, dq);
    return NULL;
  }

  mutex_init(dq->mutex);
  cond_var_init(dq->read_cond);
  dq->msg_count = 0;
  dq->head = NULL;
  dq->tail = NULL;
  dq->writing_disabled = false;
  dq->sel_read_waiters_head = NULL;
  dq->sel_write_waiters_head = NULL;

  if (err_str) {
    *err_str = NULL;
  }

  return dq;
}

/* Destroys the dynamic queue. Like __circular_queue_destroy, asserts if any
 * messages remain to make uncleaned-up data pointers visible as a bug. */
void __dynamic_queue_destroy(dynamic_queue *dq) {
  if (dq) {
    if (dynmq_msg_count(dq) > 0) {
      // The data pointers of the messages that
      // haven't been consumed are going to be
      // leaked. That's a bug on the caller side.
      // Let's make it noticed.
      ccol_assert(false);
    }

    mutex_destroy(dq->mutex);
    cond_var_destroy(dq->read_cond);
    destroy_dq_dllist(dq);

    if (dq->m_procs) {
      ccol_free_t free_func = dq->m_procs->free;
      free_func(dq->m_procs);
      free_func(dq);
    } else {
      mem_free(dq);
    }
  }
}

/* Appends msg to the dynamic queue's tail and signals read_cond. Must be
 * called with the mutex held. Returns ccol_not_enough_memory on allocation
 * failure without modifying msg->data. */
ccol_retval_t _sendto_dq(dynamic_queue *dq, c_message_t *msg) {
  ccol_retval_t retval = append_msg_to_dq_tail(dq, msg);

  if (retval == ccol_success) {
    ++dq->msg_count;
    cond_var_signal(dq->read_cond);
    notify_one_sel_waiter(dq->sel_read_waiters_head);
  }

  return retval;
}

/* Validates dynamic queue send arguments (mirrors verify_circq_send_zc_params
 * but for dynamic_queue). */
bool verify_dynmq_send_zc_params(dynamic_queue *dq, c_message_t *msg) {
  if (!dq || !msg || (msg->size == 0 && msg->data != NULL)) {
    return false;
  }

  return true;
}

/* Non-blocking send to the dynamic queue (the queue is unbounded so it never
 * waits for space). Returns ccol_not_permitted if writing is disabled, or
 * ccol_container_full if msg_count reached max_elem_count. */
ccol_retval_t dynmq_send_zc(dynamic_queue *dq, c_message_t *msg) {
  if (!verify_dynmq_send_zc_params(dq, msg)) {
    return ccol_invalid_args;
  }

  mutex_lock(dq->mutex);

  if (dq->writing_disabled) {
    mutex_unlock(dq->mutex);
    return ccol_not_permitted;
  }

  if (dq->msg_count == max_elem_count) {
    mutex_unlock(dq->mutex);
    return ccol_container_full;
  }

  ccol_retval_t result = _sendto_dq(dq, msg);

  mutex_unlock(dq->mutex);

  return result;
}

/* Removes the head message from the dynamic queue and decrements msg_count.
 * Must be called with the mutex held. */
ccol_retval_t _recvfrom_dq(dynamic_queue *dq, c_message_t *target_buf) {
  ccol_retval_t retval = remove_msg_from_dq_head(dq, target_buf);

  if (retval == ccol_success) {
    --dq->msg_count;
    notify_one_sel_waiter(dq->sel_write_waiters_head);
  }

  return retval;
}

/* Validates dynamic queue receive arguments. */
bool verify_recvfrom_dq_zc_params(dynamic_queue *dq, c_message_t *target_buf) {
  if (!dq || !target_buf) {
    return false;
  }

  return true;
}

/* Blocking receive from the dynamic queue: waits on read_cond until at least
 * one message is available, then pops it from the head. */
ccol_retval_t dynmq_recv_zc(dynamic_queue *dq, c_message_t *target_buf) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  mutex_lock(dq->mutex);

  while (dq->msg_count == 0 && !dq->writing_disabled) {
    cond_var_wait(dq->read_cond, dq->mutex);
  }

  if (dq->msg_count == 0) {
    mutex_unlock(dq->mutex);
    return ccol_not_permitted;
  }

  ccol_retval_t result = _recvfrom_dq(dq, target_buf);

  mutex_unlock(dq->mutex);

  return result;
}

/* Non-blocking receive: returns ccol_container_empty immediately when the
 * dynamic queue is empty. */
ccol_retval_t dynmq_try_recv_zc(dynamic_queue *dq, c_message_t *target_buf) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_empty;

  mutex_lock(dq->mutex);

  if (dq->msg_count > 0) {
    result = _recvfrom_dq(dq, target_buf);
  }

  mutex_unlock(dq->mutex);

  return result;
}

/* Timed receive from the dynamic queue. Same absolute-deadline approach as
 * the circular queue timed variants. */
ccol_retval_t dynmq_timed_recv_zc(dynamic_queue *dq, c_message_t *target_buf,
                                  struct timespec *timeout) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf) || !timeout) {
    return ccol_invalid_args;
  }

  mutex_lock(dq->mutex);

  if (dq->msg_count == 0) {
    int retval;
    struct timespec abs_time;
    clock_gettime(CLOCK_REALTIME, &abs_time);
    add_duration_to_timespec(&abs_time, timeout);

    while (dq->msg_count == 0 && !dq->writing_disabled) {
      if ((retval = cond_var_timedwait(dq->read_cond, dq->mutex, abs_time))) {
        if (retval != ETIMEDOUT) {
          mutex_unlock(dq->mutex);
          return ccol_unexpected_failure;
        }
        mutex_unlock(dq->mutex);
        return ccol_timed_out;
      }
    }
  }

  if (dq->msg_count == 0) {
    mutex_unlock(dq->mutex);
    return ccol_not_permitted;
  }

  ccol_retval_t result = _recvfrom_dq(dq, target_buf);

  mutex_unlock(dq->mutex);

  return result;
}

/* Sets the writing_disabled flag on the dynamic queue. Broadcasts on
 * read_cond to wake blocked receivers that are waiting on an empty queue
 * with writing now disabled. */
ccol_retval_t dynmq_disable_sending(dynamic_queue *dq) {
  if (dq) {
    mutex_lock(dq->mutex);
    dq->writing_disabled = true;
    cond_var_broadcast(dq->read_cond);  // Wake blocked receivers so they can
                                        // observe the disabled state
    notify_all_sel_waiters(dq->sel_read_waiters_head);
    notify_all_sel_waiters(dq->sel_write_waiters_head);
    mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

/* Clears the writing_disabled flag on the dynamic queue. */
ccol_retval_t dynmq_enable_sending(dynamic_queue *dq) {
  if (dq) {
    mutex_lock(dq->mutex);
    dq->writing_disabled = false;
    notify_all_sel_waiters(dq->sel_write_waiters_head);
    mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

/* Returns the number of messages in the dynamic queue. Returns
 * ccol_invalid_size if dq is NULL. */
size_t dynmq_msg_count(dynamic_queue *dq) {
  size_t result = ccol_invalid_size;

  if (dq) {
    mutex_lock(dq->mutex);
    result = dq->msg_count;
    mutex_unlock(dq->mutex);
  }

  return result;
}

// Channel related section starts here.
struct channel {
  thread_id_t owner_tid;
  circular_queue *owner_to_workers_cq;
  circular_queue *workers_to_owner_cq;
  ccol_memmgmt_procs_t *m_procs;
};

/* Creates a bidirectional channel with two circular queues: one from the owner
 * to workers, one from workers back to the owner. The channel records the
 * creating thread's ID as the owner_tid so chan_send_zc / chan_recv_zc can
 * automatically route to the correct underlying queue. */
channel *channel_create_with_mprocs(size_t max_size,
                                    ccol_memmgmt_procs_t *mmgmt_procs,
                                    char **err_str) {
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  channel *ch = (channel *)_mem_alloc(mmgmt_procs, sizeof(channel));
  if (!ch) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("Failed to allocate memory for channel");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(ch, mmgmt_procs, err_str)) {
    _mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->owner_to_workers_cq =
      circular_queue_create_with_mprocs(max_size, mmgmt_procs, err_str);
  if (!ch->owner_to_workers_cq) {
    _mem_free(mmgmt_procs, ch->m_procs);
    _mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->workers_to_owner_cq =
      circular_queue_create_with_mprocs(max_size, mmgmt_procs, err_str);
  if (!ch->workers_to_owner_cq) {
    circular_queue_destroy(ch->owner_to_workers_cq);
    _mem_free(mmgmt_procs, ch->m_procs);
    _mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->owner_tid = get_thread_id();

  if (err_str) {
    *err_str = NULL;
  }

  return ch;
}

/* Destroys both underlying circular queues then frees the channel struct. */
void __channel_destroy(channel *ch) {
  if (ch) {
    circular_queue_destroy(ch->owner_to_workers_cq);
    circular_queue_destroy(ch->workers_to_owner_cq);

    if (ch->m_procs) {
      ccol_free_t free_func = ch->m_procs->free;
      free_func(ch->m_procs);
      free_func(ch);
    } else {
      mem_free(ch);
    }
  }
}

/* Blocking send on the channel. Automatically routes to owner_to_workers_cq
 * when called from the owner thread, or workers_to_owner_cq otherwise.
 * The thread identity check is the zero-overhead routing mechanism: no explicit
 * direction parameter is needed. */
ccol_retval_t chan_send_zc(channel *ch, c_message_t *msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_send_zc(ch->owner_to_workers_cq, msg);
  }

  return circq_send_zc(ch->workers_to_owner_cq, msg);
}

/* Non-blocking channel send with automatic direction routing. */
ccol_retval_t chan_try_send_zc(channel *ch, c_message_t *msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_try_send_zc(ch->owner_to_workers_cq, msg);
  }

  return circq_try_send_zc(ch->workers_to_owner_cq, msg);
}

/* Timed channel send with automatic direction routing. */
ccol_retval_t chan_timed_send_zc(channel *ch, c_message_t *msg,
                                 struct timespec *timeout) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_timed_send_zc(ch->owner_to_workers_cq, msg, timeout);
  }

  return circq_timed_send_zc(ch->workers_to_owner_cq, msg, timeout);
}

/* Blocking channel receive with automatic direction routing. The owner thread
 * receives from workers_to_owner_cq; worker threads receive from
 * owner_to_workers_cq. */
ccol_retval_t chan_recv_zc(channel *ch, c_message_t *target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_recv_zc(ch->workers_to_owner_cq, target_buf);
  }

  return circq_recv_zc(ch->owner_to_workers_cq, target_buf);
}

/* Non-blocking channel receive with automatic direction routing. */
ccol_retval_t chan_try_recv_zc(channel *ch, c_message_t *target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_try_recv_zc(ch->workers_to_owner_cq, target_buf);
  }

  return circq_try_recv_zc(ch->owner_to_workers_cq, target_buf);
}

/* Timed channel receive with automatic direction routing. */
ccol_retval_t chan_timed_recv_zc(channel *ch, c_message_t *target_buf,
                                 struct timespec *timeout) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_timed_recv_zc(ch->workers_to_owner_cq, target_buf, timeout);
  }

  return circq_timed_recv_zc(ch->owner_to_workers_cq, target_buf, timeout);
}

/* Disables sending on the specified direction (owner_to_workers or
 * workers_to_owner). An explicit direction is required here because the caller
 * may want to disable only one side of the channel independently. */
ccol_retval_t chan_disable_sending(channel *ch, channel_direction d) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (d == owner_to_workers) {
    return circq_disable_sending(ch->owner_to_workers_cq);
  } else if (d == workers_to_owner) {
    return circq_disable_sending(ch->workers_to_owner_cq);
  }

  return ccol_invalid_args;
}

/* Re-enables sending on the specified channel direction. */
ccol_retval_t chan_enable_sending(channel *ch, channel_direction d) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (d == owner_to_workers) {
    return circq_enable_sending(ch->owner_to_workers_cq);
  } else if (d == workers_to_owner) {
    return circq_enable_sending(ch->workers_to_owner_cq);
  }

  return ccol_invalid_args;
}

/* Returns the message count for the specified direction's underlying circular
 * queue. Returns ccol_invalid_size for an unknown direction. */
size_t chan_msg_count(channel *ch, channel_direction d) {
  if (!ch) {
    return ccol_invalid_size;
  }

  if (d == owner_to_workers) {
    return circq_msg_count(ch->owner_to_workers_cq);
  } else if (d == workers_to_owner) {
    return circq_msg_count(ch->workers_to_owner_cq);
  }

  return ccol_invalid_size;
}

// ccol_select related section starts here.

/* Removes the waiter node at index i from its queue's waiter list.  Acquires
 * and releases the queue's mutex internally.  Sets nodes[i].sel_mtx to NULL
 * to mark the slot as deregistered so that subsequent calls to
 * deregister_all_sel_waiters skip it safely. */
static void deregister_sel_waiter(size_t i, ccol_sel_waiter *nodes,
                                  ccol_selectable *selectables) {
  /* fd selectables have no waiter list — nothing to unlink. */
  if (selectables[i].type == ccol_selectable_fd) {
    return;
  }

  if (selectables[i].type == ccol_selectable_circq) {
    circular_queue *cq = selectables[i].cq;
    ccol_sel_waiter **head = (selectables[i].dir == ccol_select_read)
                                 ? &cq->sel_read_waiters_head
                                 : &cq->sel_write_waiters_head;
    mutex_lock(cq->mutex);
    if (nodes[i].prev) {
      nodes[i].prev->next = nodes[i].next;
    } else {
      *head = nodes[i].next;
    }
    if (nodes[i].next) {
      nodes[i].next->prev = nodes[i].prev;
    }
    mutex_unlock(cq->mutex);
  } else {
    dynamic_queue *dq = selectables[i].dq;
    ccol_sel_waiter **head = (selectables[i].dir == ccol_select_read)
                                 ? &dq->sel_read_waiters_head
                                 : &dq->sel_write_waiters_head;
    mutex_lock(dq->mutex);
    if (nodes[i].prev) {
      nodes[i].prev->next = nodes[i].next;
    } else {
      *head = nodes[i].next;
    }
    if (nodes[i].next) {
      nodes[i].next->prev = nodes[i].prev;
    }
    mutex_unlock(dq->mutex);
  }

  /* Drain without closing: the eventfd is reused across iterations.
   * EFD_NONBLOCK is set, so this read returns EAGAIN when the counter is
   * already 0 (the producer wrote while we were processing another event).
   * The drain and the producer's write are both serialised by the queue
   * mutex, so there is no race between them. */
  if (nodes[i].efd >= 0) {
    uint64_t val;
    (void)read(nodes[i].efd, &val, sizeof(val));
  }
  nodes[i].sel_mtx = NULL;
}

/* Deregisters every node in the array whose sel_mtx is non-NULL. */
static void deregister_all_sel_waiters(size_t n, ccol_sel_waiter *nodes,
                                       ccol_selectable *selectables) {
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].sel_mtx != NULL) {
      deregister_sel_waiter(i, nodes, selectables);
    }
  }
}

/* Resolves which of a channel's two internal queues the calling thread should
 * watch, based on both thread identity and the requested direction.
 *
 * For ccol_select_read  (receive direction):
 *   owner  → workers_to_owner_cq   (mirrors chan_recv_zc)
 *   worker → owner_to_workers_cq
 *
 * For ccol_select_write (send direction):
 *   owner  → owner_to_workers_cq   (mirrors chan_send_zc)
 *   worker → workers_to_owner_cq
 */
ccol_selectable ccol_selectable_from_chan(channel *ch, ccol_select_dir dir) {
  if (!ch) {
    return (ccol_selectable){
        .type = ccol_selectable_circq, .dir = dir, .cq = NULL};
  }
  bool is_owner = (get_thread_id() == ch->owner_tid);
  circular_queue *cq;
  if (dir == ccol_select_read) {
    cq = is_owner ? ch->workers_to_owner_cq : ch->owner_to_workers_cq;
  } else {
    cq = is_owner ? ch->owner_to_workers_cq : ch->workers_to_owner_cq;
  }
  return (ccol_selectable){.type = ccol_selectable_circq, .dir = dir, .cq = cq};
}

/* Reads all currently-available data from fd into a fresh heap buffer and
 * populates msg->data and msg->size.  The caller gains ownership of msg->data
 * and is responsible for calling free() on it.
 *
 * max_bytes controls the maximum heap allocation (0 = unlimited).  For
 * non-blocking stream fds the grow loop uses max_bytes + 1 as its cap, so a
 * message of exactly max_bytes succeeds (EAGAIN fires before total > max_bytes)
 * while anything larger is caught by the "total > max_bytes" check immediately
 * after each read(2).  When the limit is exceeded, ccol_msg_too_large is
 * returned and no data is placed in msg (no partial reads).
 * For datagram fds, any datagram whose read(2) return value exceeds max_bytes
 * triggers the same error.
 *
 * Datagram vs stream handling:
 *   getsockopt(SO_TYPE) is used to detect SOCK_DGRAM and SOCK_SEQPACKET fds.
 *   For datagram sockets, read(2) delivers at most one datagram per call and
 *   silently discards any bytes that do not fit in the buffer.  To avoid
 *   silent truncation, a 66 KiB initial buffer is used — larger than the
 *   maximum standard IPv4/IPv6 UDP payload of 65,507 bytes — so a single
 *   read(2) always captures the full datagram.  The grow-retry loop is
 *   suppressed for datagram fds regardless of O_NONBLOCK, because a buffer-
 *   exactly-full result means truncation, not that more stream data follows.
 *   Non-socket fds (pipes, timerfd, etc.) and stream sockets use a 4 KiB
 *   initial buffer; if the fd has O_NONBLOCK the loop grows the buffer until
 *   EAGAIN to drain all data that arrived before the wakeup.
 *
 * Other safety properties:
 *   - fcntl(F_GETFL) is a read-only, thread-safe operation; fd flags are
 *     never modified, so concurrent users of the same fd are unaffected.
 *   - On EINTR, the read is retried automatically.
 *   - On EOF (n == 0), msg->data = NULL and msg->size = 0.
 *   - On any unexpected read error or malloc/realloc failure,
 *     ccol_unexpected_failure is returned and no heap memory is leaked. */
#define _FD_READ_STREAM_INITIAL_SIZE 4096u
#define _FD_READ_DGRAM_INITIAL_SIZE  (66u * 1024u)
static ccol_retval_t _read_from_fd(int fd, size_t max_bytes, c_message_t *msg) {
  int sock_type = 0;
  socklen_t sock_type_len = sizeof(sock_type);
  bool is_dgram =
      (getsockopt(fd, SOL_SOCKET, SO_TYPE, &sock_type, &sock_type_len) == 0) &&
      (sock_type == SOCK_DGRAM || sock_type == SOCK_SEQPACKET);

  size_t capacity = is_dgram ? _FD_READ_DGRAM_INITIAL_SIZE
                             : _FD_READ_STREAM_INITIAL_SIZE;
  void *data = malloc(capacity);
  if (!data) return ccol_unexpected_failure;

  size_t total = 0;
  /* Grow-retry loop is only safe for stream fds with O_NONBLOCK.  For
   * datagram fds, suppress it unconditionally: total == capacity would mean
   * the kernel truncated the datagram, not that more bytes follow. */
  bool nonblocking = false;
  if (!is_dgram) {
    int flags = fcntl(fd, F_GETFL, 0);
    nonblocking = (flags >= 0) && ((flags & O_NONBLOCK) != 0);
  }

  for (;;) {
    ssize_t n = read(fd, (char *)data + total, capacity - total);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      free(data);
      return ccol_unexpected_failure;
    }
    total += (size_t)n;
    if (n == 0) break;
    /* Check limit immediately after each read; applies to both dgram and
     * stream fds.  ">" rather than ">=" means a message of exactly max_bytes
     * is accepted (the equal case only reaches here when total == capacity,
     * which only matters for the grow path below). */
    if (max_bytes > 0 && total > max_bytes) {
      free(data);
      return ccol_msg_too_large;
    }
    if (total < capacity) break;
    /* Buffer exactly filled. For datagram fds (nonblocking always false here)
     * this branch is unreachable for standard datagrams with a 66 KiB buffer;
     * for stream O_NONBLOCK fds, more data may follow — grow and retry. */
    if (!nonblocking) break;
    /* Grow cap: use max_bytes + 1 as the ceiling.  The extra byte means
     * that if exactly max_bytes of data exists, the next read returns EAGAIN
     * (total < new_cap) rather than triggering a false-positive overflow.
     * Guard against SIZE_MAX overflow. */
    size_t new_cap = capacity * 2;
    if (max_bytes > 0) {
      size_t limit_cap = (max_bytes < SIZE_MAX) ? max_bytes + 1 : SIZE_MAX;
      if (new_cap > limit_cap) new_cap = limit_cap;
    }
    void *new_data = realloc(data, new_cap);
    if (!new_data) {
      free(data);
      return ccol_unexpected_failure;
    }
    data = new_data;
    capacity = new_cap;
  }

  if (total == 0) {
    free(data);
    msg->data = NULL;
    msg->size = 0;
    return ccol_success;
  }

  if (total < capacity) {
    void *trimmed = realloc(data, total);
    if (trimmed) data = trimmed;
  }

  msg->data = data;
  msg->size = total;
  return ccol_success;
}

/* Blocks until at least one of the n selectables is ready (or timeout_ms
 * elapses), then sets *ready_index and (for read-direction queue wins)
 * populates *buf.  timeout_ms == -1 means wait indefinitely.
 *
 * Locking protocol (prevents deadlock and lost wakeups):
 *
 * Each iteration is three phases:
 *
 *   Phase 1 — Per-queue (under each queue's mutex, one at a time):
 *     Read direction: check msg_count > 0.  If yes, receive immediately.
 *       If no and writing is still enabled, prepend a waiter node to the
 *       queue's sel_read_waiters_head list.  If no and writing is disabled,
 *       skip (terminal — no message will ever arrive).
 *     Write direction: check msg_count < max_size && !writing_disabled for
 *       circular_queue; !writing_disabled && msg_count < max_elem_count for
 *       dynamic_queue.  If writable, mark found immediately (buf is NOT
 *       touched; no message is consumed).  If not writable, prepend a waiter
 *       node to sel_write_waiters_head (no terminal state — keeps waiting).
 *     fd selectables: registered directly in the epoll set (see below).
 *
 *   Phase 2 — Wait:
 *     No fd selectables present: block on sel_cond under sel_mtx.  The
 *       while-loop guards against spurious wakeups and against signals that
 *       fired between Phase 1 and cond_wait.  Producers set the ready flag
 *       under sel_mtx before signalling, so no wakeup can be lost.
 *       If a deadline is set, pthread_cond_timedwait is used on a
 *       CLOCK_MONOTONIC condvar; ETIMEDOUT breaks the loop and returns
 *       ccol_timed_out.
 *     fd selectables present: block on epoll_wait(epfd).  Queue waiters
 *       carry a per-waiter eventfd (efd >= 0); notify_sel_waiters writes 1
 *       to the efd in addition to signalling sel_cond, so epoll_wait wakes
 *       for both queue and fd events.  If a user fd fires, return immediately.
 *       If a queue eventfd fires, fall through to Phase 3 and re-evaluate.
 *       When a deadline is set, the remaining time is recomputed before each
 *       epoll_wait call (including after EINTR retries) so EINTR cannot
 *       extend the timeout.  epoll_wait returning 0 means the deadline
 *       elapsed; ccol_timed_out is returned.
 *
 *   Phase 3 — Deregister:
 *     Re-acquire each queue's mutex, splice the node out of the correct waiter
 *     list, drain each open eventfd with a non-blocking read (resetting its
 *     counter to 0 for the next iteration), and loop back to Phase 1.
 *
 * Lock ordering: producers take (queue mutex → sel_mtx). ccol_select takes each
 * queue mutex alone in Phase 1 and Phase 3, and sel_mtx alone in Phase 2
 * (condvar path).  epoll_wait holds no application-level locks.  No two locks
 * are ever held simultaneously, so there is no lock-ordering cycle.
 *
 * Node lifetime: producer notification happens while the producer holds the
 * queue mutex.  Deregistration also requires the queue mutex.  Therefore a
 * node cannot be removed from the list while a producer is traversing it —
 * the heap node cannot vanish mid-traversal.
 *
 * epfd lifecycle: created once per call before the loop.  User fds are added
 * once with EPOLL_CTL_ADD before the loop and kept registered for the entire
 * call; level-triggered semantics ensure a ready fd continues to fire on every
 * epoll_wait until the caller consumes it.  Closing epfd on return removes
 * them automatically — no per-iteration DEL/ADD needed.  Queue eventfds are
 * created per-waiter on the first registration and reused across iterations:
 * deregister_sel_waiter drains them (non-blocking read) instead of closing,
 * so Phase 1 re-links the existing node without any new epoll_ctl or eventfd
 * syscalls. */
ccol_retval_t ccol_select_timed(c_message_t *buf, size_t *ready_index, size_t n,
                                ccol_selectable *selectables, int timeout_ms) {
  if (!buf || !ready_index || n == 0 || !selectables) {
    return ccol_invalid_args;
  }
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == ccol_selectable_circq) {
      if (!selectables[i].cq) return ccol_invalid_args;
    } else if (selectables[i].type == ccol_selectable_dynq) {
      if (!selectables[i].dq) return ccol_invalid_args;
    } else if (selectables[i].type == ccol_selectable_fd) {
      if (selectables[i].fd < 0) return ccol_invalid_args;
    } else {
      return ccol_invalid_args;
    }
    if (selectables[i].dir != ccol_select_read &&
        selectables[i].dir != ccol_select_write) {
      return ccol_invalid_args;
    }
  }

  /* Determine whether any fd selectables are present once; this drives the
   * choice between the condvar path (no overhead) and the epoll path. */
  bool has_fd_sels = false;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == ccol_selectable_fd) {
      has_fd_sels = true;
      break;
    }
  }

  /* One waiter node per selectable.  sel_mtx == NULL means "not currently
   * registered in any queue's list."  efd == -1 means "no eventfd allocated"
   * (condvar-only mode).  Heap-allocated to avoid stack overflow for large n. */
  ccol_sel_waiter *nodes = malloc(n * sizeof(ccol_sel_waiter));
  if (!nodes) return ccol_not_enough_memory;
  for (size_t i = 0; i < n; i++) {
    nodes[i].sel_mtx = NULL;
    nodes[i].efd = -1;
  }

  mutex_t sel_mtx;
  cond_var_t sel_cond;
  bool ready = false;
  mutex_init(sel_mtx);
  /* Always initialise with CLOCK_MONOTONIC so timed waits are immune to
   * wall-clock adjustments.  Infinite waits (cond_var_wait) ignore the clock
   * attribute so this is safe even when no timeout is used. */
  {
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&sel_cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);
  }

  /* Compute the absolute CLOCK_MONOTONIC deadline once before the loop so
   * that repeated spurious wakeups or EINTR retries cannot extend the timeout.
   * has_deadline == false means wait indefinitely (timeout_ms == -1). */
  bool has_deadline = (timeout_ms >= 0);
  struct timespec deadline = {0, 0};
  if (has_deadline) {
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }
  }

  int epfd = -1;
  if (has_fd_sels) {
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
      free(nodes);
      mutex_destroy(sel_mtx);
      cond_var_destroy(sel_cond);
      return ccol_unexpected_failure;
    }
  }

  ccol_retval_t retval = ccol_not_permitted;

  /* In epoll mode, register all user fds in the epoll set once before the
   * retry loop.  Level-triggered epoll keeps a ready fd firing on every
   * epoll_wait call until the caller consumes it, so no per-iteration
   * ADD/DEL is needed.  Closing epfd on return auto-removes every entry.
   * No queue waiters are registered yet, so a failure here can return
   * directly without going through cleanup_unexpected_failure. */
  size_t fd_sel_count = 0;
  if (has_fd_sels) {
    for (size_t i = 0; i < n; i++) {
      if (selectables[i].type != ccol_selectable_fd) continue;
      struct epoll_event ev;
      ev.data.u64 = (uint64_t)i;
      ev.events = (selectables[i].dir == ccol_select_read)
                      ? (uint32_t)(EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)
                      : (uint32_t)(EPOLLOUT | EPOLLERR | EPOLLHUP);
      if (epoll_ctl(epfd, EPOLL_CTL_ADD, selectables[i].fd, &ev) < 0) {
        close(epfd);
        free(nodes);
        mutex_destroy(sel_mtx);
        cond_var_destroy(sel_cond);
        return ccol_unexpected_failure;
      }
      fd_sel_count++;
    }
  }

  for (;;) {
    /* === Phase 1: scan + register === */
    int found = -1;
    size_t registered = 0;

    for (size_t i = 0; i < n && found < 0; i++) {
      if (selectables[i].type == ccol_selectable_fd) continue;

      if (selectables[i].type == ccol_selectable_circq) {
        circular_queue *cq = selectables[i].cq;
        mutex_lock(cq->mutex);

        if (selectables[i].dir == ccol_select_read) {
          if (cq->msg_count > 0) {
            _recvfrom_cq(cq, buf);
            /* Cascade: if more messages remain and other read waiters are
             * registered, wake one so it can claim the next message.  This
             * prevents a liveness gap when the producer woke only this thread
             * (the head) for multiple messages. */
            if (cq->msg_count > 0 && cq->sel_read_waiters_head) {
              notify_one_sel_waiter(cq->sel_read_waiters_head);
            }
            mutex_unlock(cq->mutex);
            found = (int)i;
          } else if (!cq->writing_disabled) {
            /* epoll mode: allocate the eventfd and register it with epoll on
             * the first iteration; on subsequent iterations the existing efd
             * is reused (already in epoll) so only the list-link below runs. */
            if (has_fd_sels && nodes[i].efd < 0) {
              nodes[i].efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
              if (nodes[i].efd < 0) {
                mutex_unlock(cq->mutex);
                goto cleanup_unexpected_failure;
              }
              struct epoll_event ev = {.data.u64 = (uint64_t)i,
                                       .events = EPOLLIN};
              if (epoll_ctl(epfd, EPOLL_CTL_ADD, nodes[i].efd, &ev) < 0) {
                close(nodes[i].efd);
                nodes[i].efd = -1;
                mutex_unlock(cq->mutex);
                goto cleanup_unexpected_failure;
              }
            }
            nodes[i].sel_mtx = &sel_mtx;
            nodes[i].sel_cond = &sel_cond;
            nodes[i].ready = &ready;
            nodes[i].prev = NULL;
            nodes[i].next = cq->sel_read_waiters_head;
            if (cq->sel_read_waiters_head) {
              cq->sel_read_waiters_head->prev = &nodes[i];
            }
            cq->sel_read_waiters_head = &nodes[i];
            registered++;
            mutex_unlock(cq->mutex);
          } else {
            /* empty + writing disabled: terminal for this selectable */
            mutex_unlock(cq->mutex);
          }
        } else {
          /* ccol_select_write: writable if there is room and sending is on */
          if (cq->msg_count < cq->max_size && !cq->writing_disabled) {
            /* Cascade: if more slots remain and other write waiters are
             * registered, wake one.  The cascade is self-limiting: if the
             * queue fills by the time the woken thread reaches Phase 1, it
             * re-registers rather than cascading further. */
            if (cq->msg_count + 1 < cq->max_size && cq->sel_write_waiters_head) {
              notify_one_sel_waiter(cq->sel_write_waiters_head);
            }
            mutex_unlock(cq->mutex);
            found = (int)i;
          } else {
            if (has_fd_sels && nodes[i].efd < 0) {
              nodes[i].efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
              if (nodes[i].efd < 0) {
                mutex_unlock(cq->mutex);
                goto cleanup_unexpected_failure;
              }
              struct epoll_event ev = {.data.u64 = (uint64_t)i,
                                       .events = EPOLLIN};
              if (epoll_ctl(epfd, EPOLL_CTL_ADD, nodes[i].efd, &ev) < 0) {
                close(nodes[i].efd);
                nodes[i].efd = -1;
                mutex_unlock(cq->mutex);
                goto cleanup_unexpected_failure;
              }
            }
            nodes[i].sel_mtx = &sel_mtx;
            nodes[i].sel_cond = &sel_cond;
            nodes[i].ready = &ready;
            nodes[i].prev = NULL;
            nodes[i].next = cq->sel_write_waiters_head;
            if (cq->sel_write_waiters_head) {
              cq->sel_write_waiters_head->prev = &nodes[i];
            }
            cq->sel_write_waiters_head = &nodes[i];
            registered++;
            mutex_unlock(cq->mutex);
          }
        }
      } else {
        /* ccol_selectable_dynq */
        dynamic_queue *dq = selectables[i].dq;
        mutex_lock(dq->mutex);

        if (selectables[i].dir == ccol_select_read) {
          if (dq->msg_count > 0) {
            if (_recvfrom_dq(dq, buf) != ccol_success) {
              /* Invariant violation: msg_count > 0 was verified under the lock.
               * _recvfrom_dq only fails when the list is empty, which cannot be
               * true here. Crash loudly rather than silently corrupt state. */
              fatal_err("ccol_select: _recvfrom_dq invariant violation");
            }
            /* Cascade: wake one more read waiter if messages remain. */
            if (dq->msg_count > 0 && dq->sel_read_waiters_head) {
              notify_one_sel_waiter(dq->sel_read_waiters_head);
            }
            mutex_unlock(dq->mutex);
            found = (int)i;
          } else if (!dq->writing_disabled) {
            if (has_fd_sels && nodes[i].efd < 0) {
              nodes[i].efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
              if (nodes[i].efd < 0) {
                mutex_unlock(dq->mutex);
                goto cleanup_unexpected_failure;
              }
              struct epoll_event ev = {.data.u64 = (uint64_t)i,
                                       .events = EPOLLIN};
              if (epoll_ctl(epfd, EPOLL_CTL_ADD, nodes[i].efd, &ev) < 0) {
                close(nodes[i].efd);
                nodes[i].efd = -1;
                mutex_unlock(dq->mutex);
                goto cleanup_unexpected_failure;
              }
            }
            nodes[i].sel_mtx = &sel_mtx;
            nodes[i].sel_cond = &sel_cond;
            nodes[i].ready = &ready;
            nodes[i].prev = NULL;
            nodes[i].next = dq->sel_read_waiters_head;
            if (dq->sel_read_waiters_head) {
              dq->sel_read_waiters_head->prev = &nodes[i];
            }
            dq->sel_read_waiters_head = &nodes[i];
            registered++;
            mutex_unlock(dq->mutex);
          } else {
            /* empty + writing disabled: terminal for this selectable */
            mutex_unlock(dq->mutex);
          }
        } else {
          /* ccol_select_write: writable unless writing_disabled or at capacity */
          if (!dq->writing_disabled && dq->msg_count < max_elem_count) {
            /* Cascade: wake one more write waiter; the cascade self-limits
             * at max_elem_count just as the read cascade self-limits at 0. */
            if (dq->msg_count + 1 < max_elem_count && dq->sel_write_waiters_head) {
              notify_one_sel_waiter(dq->sel_write_waiters_head);
            }
            mutex_unlock(dq->mutex);
            found = (int)i;
          } else {
            if (has_fd_sels && nodes[i].efd < 0) {
              nodes[i].efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
              if (nodes[i].efd < 0) {
                mutex_unlock(dq->mutex);
                goto cleanup_unexpected_failure;
              }
              struct epoll_event ev = {.data.u64 = (uint64_t)i,
                                       .events = EPOLLIN};
              if (epoll_ctl(epfd, EPOLL_CTL_ADD, nodes[i].efd, &ev) < 0) {
                close(nodes[i].efd);
                nodes[i].efd = -1;
                mutex_unlock(dq->mutex);
                goto cleanup_unexpected_failure;
              }
            }
            nodes[i].sel_mtx = &sel_mtx;
            nodes[i].sel_cond = &sel_cond;
            nodes[i].ready = &ready;
            nodes[i].prev = NULL;
            nodes[i].next = dq->sel_write_waiters_head;
            if (dq->sel_write_waiters_head) {
              dq->sel_write_waiters_head->prev = &nodes[i];
            }
            dq->sel_write_waiters_head = &nodes[i];
            registered++;
            mutex_unlock(dq->mutex);
          }
        }
      }
    }

    if (found >= 0) {
      deregister_all_sel_waiters(n, nodes, selectables);
      *ready_index = (size_t)found;
      retval = ccol_success;
      break;
    }

    if (registered == 0 && fd_sel_count == 0) {
      /* Every read-direction queue selectable was empty with writing
       * disabled, and there are no fd selectables to wait on.  No future
       * event can unblock us. */
      retval = ccol_not_permitted;
      break;
    }

    /* === Phase 2: wait === */
    if (!has_fd_sels) {
      /* Condvar path: the while-loop guards against spurious wakeups and
       * against signals that arrived after Phase 1 but before cond_wait.
       * When a deadline is set, pthread_cond_timedwait is used; ETIMEDOUT
       * with ready still false means no selectable fired in time. */
      mutex_lock(sel_mtx);
      if (has_deadline) {
        bool timed_out_flag = false;
        while (!ready) {
          int wait_ret =
              pthread_cond_timedwait(&sel_cond, &sel_mtx, &deadline);
          if (wait_ret == ETIMEDOUT) {
            /* Check ready once more: a producer may have set it between the
             * timeout expiry and our re-acquisition of sel_mtx. */
            if (!ready) {
              timed_out_flag = true;
            }
            break;
          }
          /* 0 or EINTR: re-check ready in the loop condition. */
        }
        ready = false;
        mutex_unlock(sel_mtx);
        if (timed_out_flag) {
          /* Deregister before breaking: the nodes are still linked into the
           * queues' waiter lists; freeing them without unlinking first would
           * leave dangling pointers that any subsequent producer would
           * dereference → use-after-free. */
          deregister_all_sel_waiters(n, nodes, selectables);
          retval = ccol_timed_out;
          break;
        }
      } else {
        while (!ready) {
          cond_var_wait(sel_cond, sel_mtx);
        }
        ready = false;
        mutex_unlock(sel_mtx);
      }
    } else {
      /* Epoll path: block until any registered fd or queue eventfd is ready.
       * When a deadline is set, the remaining time is recomputed before each
       * epoll_wait call so EINTR retries do not extend the timeout.
       * n_ready == 0 means the deadline elapsed → ccol_timed_out. */
      struct epoll_event ev;
      int n_ready;
      int epoll_to;
      do {
        if (has_deadline) {
          struct timespec now;
          clock_gettime(CLOCK_MONOTONIC, &now);
          long long remaining_ms =
              ((long long)(deadline.tv_sec - now.tv_sec)) * 1000LL +
              ((long long)(deadline.tv_nsec - now.tv_nsec)) / 1000000LL;
          if (remaining_ms <= 0) {
            n_ready = 0;
            break;
          }
          epoll_to = (remaining_ms > INT_MAX) ? INT_MAX : (int)remaining_ms;
        } else {
          epoll_to = -1;
        }
        n_ready = epoll_wait(epfd, &ev, 1, epoll_to);
      } while (n_ready < 0 && errno == EINTR);

      if (n_ready == 0) {
        /* Deregister before breaking: same use-after-free risk as the condvar
         * timeout path above — queue waiter lists still hold pointers to nodes
         * that are about to be freed. */
        deregister_all_sel_waiters(n, nodes, selectables);
        retval = ccol_timed_out;
        break;
      }

      if (n_ready < 0) {
        goto cleanup_unexpected_failure;
      }

      size_t fired_idx = (size_t)ev.data.u64;
      if (selectables[fired_idx].type == ccol_selectable_fd) {
        /* A user fd became ready.  Deregister queue waiters before touching
         * buf so that any concurrent producer still in-flight finds no
         * waiter node to notify (preventing a stale write to a freed heap
         * node).  User fd entries are removed automatically when epfd is
         * closed. */
        deregister_all_sel_waiters(n, nodes, selectables);
        if (selectables[fired_idx].dir == ccol_select_read) {
          retval = _read_from_fd(selectables[fired_idx].fd,
                                 selectables[fired_idx].max_fd_read_bytes, buf);
        } else {
          /* Write-direction fd win: symmetric with write-direction queue
           * wins — buf is left untouched; the caller calls write(2). */
          retval = ccol_success;
        }
        if (retval == ccol_success) {
          *ready_index = fired_idx;
        }
        break;
      }
      /* A queue eventfd fired.  Fall through to Phase 3 so we deregister
       * all waiters and loop back to Phase 1 to re-evaluate. */
    }

    /* === Phase 3: deregister, then loop back to Phase 1 ===
     * deregister_sel_waiter drains each open eventfd with a non-blocking
     * read (resetting its counter to 0); the eventfd stays registered in
     * epoll so Phase 1 re-links the node without any new syscalls.
     * User fds remain registered in epoll for the same reason. */
    deregister_all_sel_waiters(n, nodes, selectables);
  }

  /* Close all eventfds allocated across iterations; closing epfd
   * auto-removes all user fds and queue eventfds from the epoll set. */
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].efd >= 0) close(nodes[i].efd);
  }
  if (epfd >= 0) close(epfd);
  mutex_destroy(sel_mtx);
  cond_var_destroy(sel_cond);
  free(nodes);
  return retval;

cleanup_unexpected_failure:
  /* Reached only in epoll mode when a syscall (eventfd, epoll_ctl) fails.
   * Deregister queue waiters linked before the failure, close any eventfds
   * that were allocated, then close epfd (auto-removes user fds and any
   * eventfd entries). */
  deregister_all_sel_waiters(n, nodes, selectables);
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].efd >= 0) close(nodes[i].efd);
  }
  if (epfd >= 0) close(epfd);
  mutex_destroy(sel_mtx);
  cond_var_destroy(sel_cond);
  free(nodes);
  return ccol_unexpected_failure;
}

ccol_retval_t ccol_select(c_message_t *buf, size_t *ready_index, size_t n,
                          ccol_selectable *selectables) {
  return ccol_select_timed(buf, ready_index, n, selectables, -1);
}
