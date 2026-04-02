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

#include <chashmap.h>
#include <cthreadcomm.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#ifdef RUNNING_UNIT_TESTS
#include <assert.h>
#endif

/* Adds duration to target, normalising tv_nsec into [0, 1e9) to keep the
 * struct in a valid state for cond_var_timedwait. Both inputs are
 * normalised independently so the function is safe even when either
 * carries an already-overflowed tv_nsec. */
void add_duration_to_timespec(struct timespec *target,
                              struct timespec *duration) {
  static const long int max_nsecs = 1000000000;

  if (target->tv_nsec >= max_nsecs) {
    target->tv_sec += target->tv_nsec / max_nsecs;
    target->tv_nsec = target->tv_nsec % max_nsecs;
  }

  /* local copy; avoid mutating caller's struct */
  struct timespec dur = *duration;
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
  mutex_t *sel_mtx;
  cond_var_t *sel_cond;
  bool *ready;
  int efd; /* eventfd for epoll mode; -1 in condvar-only mode */
  struct ccol_sel_waiter *prev;
  struct ccol_sel_waiter *next;
} ccol_sel_waiter;

/* Wakes a single waiter node.  Must be called while the owning queue's mutex
 * is held so that the node pointer remains valid throughout. */
static void _notify_waiter(ccol_sel_waiter *w) {
  mutex_lock(*w->sel_mtx);
  *w->ready = true;
  mutex_unlock(*w->sel_mtx);
  cond_var_signal(*w->sel_cond);
  if (w->efd >= 0) {
    uint64_t one = 1;
    (void)write(w->efd, &one, sizeof(one));
  }
}

/* Wakes the single head waiter on a queue.  Used for message/slot events where
 * exactly one resource became available; waking more than one waiter would
 * cause a thundering herd.  The cascade mechanism inside ccol_select's Phase 1
 * propagates the wake further when additional resources remain after the first
 * woken thread claims its resource.  Must be called while the queue's own mutex
 * is held. */
static void notify_one_sel_waiter(ccol_sel_waiter *head) {
  if (head) _notify_waiter(head);
}

/* Wakes every thread currently blocked in ccol_select on this queue.  Used
 * exclusively for state-change events (disable_sending, enable_sending) where
 * every blocked thread must re-evaluate regardless of resource availability.
 * Must be called while the queue's own mutex is held. */
static void notify_all_sel_waiters(ccol_sel_waiter *head) {
  for (ccol_sel_waiter *w = head; w != NULL; w = w->next) _notify_waiter(w);
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
 * because their data pointers would be leaked; that is a bug on the caller's
 * side and must be made visible. */
void __circular_queue_destroy(circular_queue *cq) {
  if (cq) {
    if (circq_msg_count(cq) > 0) {
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
  msg->data = NULL;
  if (cq->write_index == cq->max_size) {
    cq->write_index = 0;
  }
  ++cq->msg_count;

  cond_var_signal(cq->read_cond);
  notify_one_sel_waiter(cq->sel_read_waiters_head);
}

/* Validates send arguments: the queue and message must be non-NULL, and a
 * message must have a consistent data/size pair: data != NULL requires size > 0
 * (no empty payload with a live pointer), and data == NULL requires size == 0
 * (NULL with a non-zero size is an inconsistent sentinel). */
bool verify_circq_send_zc_params(circular_queue *cq, c_message_t *msg) {
  if (!cq || !msg || (msg->size == 0 && msg->data != NULL) ||
      (msg->data == NULL && msg->size != 0)) {
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

  ccol_retval_t result = ccol_container_full;

  mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  if (cq->msg_count < cq->max_size) {
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
        /* Re-check under the mutex: a consumer may have freed a slot between
         * the kernel detecting the expiry and us reacquiring the mutex.
         * Also re-check writing_disabled for the same reason. */
        if (cq->msg_count < cq->max_size || cq->writing_disabled) break;
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

  while (cq->msg_count == 0) {
    cond_var_wait(cq->read_cond, cq->mutex);
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

    while (cq->msg_count == 0) {
      if ((retval = cond_var_timedwait(cq->read_cond, cq->mutex, abs_time))) {
        if (retval != ETIMEDOUT) {
          mutex_unlock(cq->mutex);
          return ccol_unexpected_failure;
        }
        /* Re-check under the mutex: a producer may have added a message between
         * the kernel detecting the expiry and us reacquiring the mutex. */
        if (cq->msg_count > 0) break;
        mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
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
    cond_var_broadcast(cq->write_cond);
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
    cond_var_broadcast(cq->write_cond);
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

/* Dynamic queue related section starts here. */
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
    dq->tail = NULL;
  }

  _mem_free(dq->m_procs, node_to_be_freed);
  return ccol_success;
}

/* Frees all dllist_node structs in the dynamic queue. Does not free the data
 * pointers stored in each message; those should have already been consumed
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
  if (!dq || !msg || (msg->size == 0 && msg->data != NULL) ||
      (msg->data == NULL && msg->size != 0)) {
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

  while (dq->msg_count == 0) {
    cond_var_wait(dq->read_cond, dq->mutex);
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

    while (dq->msg_count == 0) {
      if ((retval = cond_var_timedwait(dq->read_cond, dq->mutex, abs_time))) {
        if (retval != ETIMEDOUT) {
          mutex_unlock(dq->mutex);
          return ccol_unexpected_failure;
        }
        /* Re-check under the mutex: a producer may have added a message between
         * the kernel detecting the expiry and us reacquiring the mutex. */
        if (dq->msg_count > 0) break;
        mutex_unlock(dq->mutex);
        return ccol_timed_out;
      }
    }
  }

  ccol_retval_t result = _recvfrom_dq(dq, target_buf);

  mutex_unlock(dq->mutex);

  return result;
}

/* Sets the writing_disabled flag on the dynamic queue. */
ccol_retval_t dynmq_disable_sending(dynamic_queue *dq) {
  if (dq) {
    mutex_lock(dq->mutex);
    dq->writing_disabled = true;
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

/* Channel related section starts here. */
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

/* ccol_select related section starts here. */

/* Splices node out of a waiter doubly-linked list under the queue's mutex.
 * head must be the address of the appropriate sel_{read,write}_waiters_head
 * pointer in the owning queue. */
static void _sel_unlink_waiter(ccol_sel_waiter *node, ccol_sel_waiter **head,
                               mutex_t *mtx) {
  mutex_lock(*mtx);
  if (node->prev)
    node->prev->next = node->next;
  else
    *head = node->next;
  if (node->next) node->next->prev = node->prev;
  mutex_unlock(*mtx);
}

/* Removes the waiter node at index i from its queue's waiter list.  Acquires
 * and releases the queue's mutex internally.  Sets nodes[i].sel_mtx to NULL
 * to mark the slot as deregistered so that subsequent calls to
 * deregister_all_sel_waiters skip it safely. */
static void deregister_sel_waiter(size_t i, ccol_sel_waiter *nodes,
                                  ccol_selectable *selectables) {
  /* fd selectables have no waiter list; nothing to unlink. */
  if (selectables[i].type == ccol_selectable_fd) return;

  if (selectables[i].type == ccol_selectable_circq) {
    circular_queue *cq = selectables[i].cq;
    ccol_sel_waiter **head = (selectables[i].dir == ccol_select_read)
                                 ? &cq->sel_read_waiters_head
                                 : &cq->sel_write_waiters_head;
    _sel_unlink_waiter(&nodes[i], head, &cq->mutex);
  } else {
    dynamic_queue *dq = selectables[i].dq;
    ccol_sel_waiter **head = (selectables[i].dir == ccol_select_read)
                                 ? &dq->sel_read_waiters_head
                                 : &dq->sel_write_waiters_head;
    _sel_unlink_waiter(&nodes[i], head, &dq->mutex);
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
 *   owner  reads from workers_to_owner_cq   (mirrors chan_recv_zc)
 *   worker reads from owner_to_workers_cq
 *
 * For ccol_select_write (send direction):
 *   owner  writes to owner_to_workers_cq   (mirrors chan_send_zc)
 *   worker writes to workers_to_owner_cq
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

/* Allocates an eventfd for nodes[i] (if not already present) and registers it
 * with epfd under EPOLLIN.  The eventfd is allocated once and reused across
 * loop iterations; subsequent calls with nodes[i].efd >= 0 are no-ops.
 * Returns true on success or false on any system error (eventfd/epoll_ctl). */
static bool _sel_ensure_efd(size_t i, ccol_sel_waiter *nodes, int epfd) {
  if (nodes[i].efd >= 0) return true;
  nodes[i].efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (nodes[i].efd < 0) return false;
  struct epoll_event ev = {.data.u64 = (uint64_t)i, .events = EPOLLIN};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, nodes[i].efd, &ev) < 0) {
    close(nodes[i].efd);
    nodes[i].efd = -1;
    return false;
  }
  return true;
}

/* Fills in nodes[i] and prepends it to *head.  Called under the owning queue's
 * mutex; the caller unlocks after this returns. */
static void _sel_link_waiter(size_t i, ccol_sel_waiter *nodes, mutex_t *sel_mtx,
                             cond_var_t *sel_cond, bool *ready,
                             ccol_sel_waiter **head) {
  nodes[i].sel_mtx = sel_mtx;
  nodes[i].sel_cond = sel_cond;
  nodes[i].ready = ready;
  nodes[i].prev = NULL;
  nodes[i].next = *head;
  if (*head) (*head)->prev = &nodes[i];
  *head = &nodes[i];
}

/* Returns ccol_success if all arguments are valid, ccol_invalid_args otherwise.
 */
static ccol_retval_t _sel_validate_args(const size_t *ready_index, size_t n,
                                        const ccol_selectable *selectables) {
  if (!ready_index || n == 0 || !selectables) return ccol_invalid_args;
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
        selectables[i].dir != ccol_select_write)
      return ccol_invalid_args;
  }
  return ccol_success;
}

/* Computes an absolute CLOCK_MONOTONIC deadline from timeout_ms.  Returns true
 * and fills *deadline when timeout_ms >= 0; returns false (infinite wait) when
 * timeout_ms < 0. */
static bool _sel_compute_deadline(int timeout_ms, struct timespec *deadline) {
  if (timeout_ms < 0) return false;
  clock_gettime(CLOCK_MONOTONIC, deadline);
  deadline->tv_sec += timeout_ms / 1000;
  deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec++;
    deadline->tv_nsec -= 1000000000L;
  }
  return true;
}

/* Creates an epoll instance and registers every fd selectable with
 * level-triggered interest flags.  Returns the epfd on success or -1 on any
 * system error.  Closing the returned epfd auto-removes all registered fds. */
static int _sel_setup_epoll(size_t n, ccol_selectable *selectables) {
  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) return -1;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type != ccol_selectable_fd) continue;
    struct epoll_event ev;
    ev.data.u64 = (uint64_t)i;
    ev.events = (selectables[i].dir == ccol_select_read)
                    ? (uint32_t)(EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)
                    : (uint32_t)(EPOLLOUT | EPOLLERR | EPOLLHUP);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, selectables[i].fd, &ev) < 0) {
      close(epfd);
      return -1;
    }
  }
  return epfd;
}

/* Scans every non-fd selectable for readiness and links waiter nodes into
 * queue lists for those not yet ready.  Returns the found index (>= 0) when a
 * ready selectable is detected and its resource consumed or slot confirmed, -1
 * when no selectable was ready and all waiters are now registered, or -2 on a
 * system error (eventfd/epoll_ctl).  On -2 the faulting queue's mutex has
 * already been released; previously registered nodes remain linked and the
 * caller must call deregister_all_sel_waiters before freeing them. */
static int _sel_phase1_scan_register(size_t n, ccol_selectable *selectables,
                                     ccol_sel_waiter *nodes, bool has_fd_sels,
                                     int epfd, mutex_t *sel_mtx,
                                     cond_var_t *sel_cond, bool *ready) {
  int found = -1;
  for (size_t i = 0; i < n && found < 0; i++) {
    if (selectables[i].type == ccol_selectable_fd) continue;

    if (selectables[i].type == ccol_selectable_circq) {
      circular_queue *cq = selectables[i].cq;
      mutex_lock(cq->mutex);

      if (selectables[i].dir == ccol_select_read) {
        /* Peek only; ccol_select() never consumes, the caller performs its
         * own explicit circq_try_recv_zc() afterward. Forward the notify to
         * the next waiter unconditionally (mirroring the write-direction
         * branch below exactly), not contingent on messages remaining after
         * a consume this function no longer performs: a thread about to
         * leave the waiter list has no other way to guarantee the next
         * waiter learns the condition is still true. Omitting this would
         * reintroduce the class of starvation bug
         * write_circq_two_concurrent_waiters_both_wake_on_slot_free guards
         * against, on the read-direction side. */
        if (cq->msg_count > 0) {
          if (cq->sel_read_waiters_head)
            notify_one_sel_waiter(cq->sel_read_waiters_head);
          mutex_unlock(cq->mutex);
          found = (int)i;
        } else {
          if (has_fd_sels && !_sel_ensure_efd(i, nodes, epfd)) {
            mutex_unlock(cq->mutex);
            return -2;
          }
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &cq->sel_read_waiters_head);
          mutex_unlock(cq->mutex);
        }
      } else {
        /* ccol_select_write: writable if there is room and sending is on */
        if (cq->msg_count < cq->max_size && !cq->writing_disabled) {
          if (cq->sel_write_waiters_head)
            notify_one_sel_waiter(cq->sel_write_waiters_head);
          mutex_unlock(cq->mutex);
          found = (int)i;
        } else {
          if (has_fd_sels && !_sel_ensure_efd(i, nodes, epfd)) {
            mutex_unlock(cq->mutex);
            return -2;
          }
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &cq->sel_write_waiters_head);
          mutex_unlock(cq->mutex);
        }
      }
    } else {
      /* ccol_selectable_dynq */
      dynamic_queue *dq = selectables[i].dq;
      mutex_lock(dq->mutex);

      if (selectables[i].dir == ccol_select_read) {
        /* Peek only, same reasoning as the circq read branch above. */
        if (dq->msg_count > 0) {
          if (dq->sel_read_waiters_head)
            notify_one_sel_waiter(dq->sel_read_waiters_head);
          mutex_unlock(dq->mutex);
          found = (int)i;
        } else {
          if (has_fd_sels && !_sel_ensure_efd(i, nodes, epfd)) {
            mutex_unlock(dq->mutex);
            return -2;
          }
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &dq->sel_read_waiters_head);
          mutex_unlock(dq->mutex);
        }
      } else {
        /* ccol_select_write: writable unless writing_disabled or at capacity */
        if (!dq->writing_disabled && dq->msg_count < max_elem_count) {
          if (dq->sel_write_waiters_head)
            notify_one_sel_waiter(dq->sel_write_waiters_head);
          mutex_unlock(dq->mutex);
          found = (int)i;
        } else {
          if (has_fd_sels && !_sel_ensure_efd(i, nodes, epfd)) {
            mutex_unlock(dq->mutex);
            return -2;
          }
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &dq->sel_write_waiters_head);
          mutex_unlock(dq->mutex);
        }
      }
    }
  }
  return found;
}

/* Waits on sel_cond until *ready is set by a producer or the deadline elapses.
 * Returns true if the deadline elapsed with *ready still false; false on a
 * normal wakeup.  Always resets *ready to false before returning. */
static bool _sel_wait_condvar(mutex_t *sel_mtx, cond_var_t *sel_cond,
                              bool *ready, bool has_deadline,
                              const struct timespec *deadline) {
  mutex_lock(*sel_mtx);
  if (has_deadline) {
    bool timed_out_flag = false;
    while (!*ready) {
      int wait_ret = cond_var_timedwait(*sel_cond, *sel_mtx, *deadline);
      if (wait_ret == ETIMEDOUT) {
        if (!*ready) timed_out_flag = true;
        break;
      }
    }
    *ready = false;
    mutex_unlock(*sel_mtx);
    return timed_out_flag;
  }
  while (!*ready) cond_var_wait(*sel_cond, *sel_mtx);
  *ready = false;
  mutex_unlock(*sel_mtx);
  return false;
}

typedef enum {
  _SEL_EPOLL_CONTINUE, /* queue eventfd fired; fall through to Phase 3 */
  _SEL_EPOLL_BREAK,    /* done or timed out; *out_retval and *ready_index set */
  _SEL_EPOLL_FAILURE,  /* unexpected system error; nodes still registered */
} _sel_epoll_outcome;

/* Blocks on epoll_wait until any registered descriptor is ready or the deadline
 * elapses.  Returns _SEL_EPOLL_CONTINUE when a queue eventfd fires (the caller
 * runs Phase 3 and loops back to Phase 1), _SEL_EPOLL_BREAK when the overall
 * result is determined (*out_retval and *ready_index are set by this function),
 * or _SEL_EPOLL_FAILURE on a system error (nodes remain registered; the caller
 * must deregister before freeing). */
static _sel_epoll_outcome _sel_wait_epoll(
    int epfd, bool has_deadline, const struct timespec *deadline, size_t n,
    ccol_selectable *selectables, ccol_sel_waiter *nodes, size_t *ready_index,
    ccol_retval_t *out_retval) {
  struct epoll_event ev;
  int n_ready;
  int epoll_to;
  do {
    if (has_deadline) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long long remaining_ms =
          ((long long)(deadline->tv_sec - now.tv_sec)) * 1000LL +
          ((long long)(deadline->tv_nsec - now.tv_nsec)) / 1000000LL;
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
    deregister_all_sel_waiters(n, nodes, selectables);
    *out_retval = ccol_timed_out;
    return _SEL_EPOLL_BREAK;
  }
  if (n_ready < 0) return _SEL_EPOLL_FAILURE;

  size_t fired_idx = (size_t)ev.data.u64;
  if (selectables[fired_idx].type == ccol_selectable_fd) {
    deregister_all_sel_waiters(n, nodes, selectables);
    /* Readiness only, for either direction: ccol_select() never reads or
     * writes the fd itself, the caller performs its own read(2)/recv(2) or
     * write(2)/send(2) afterward. */
    *out_retval = ccol_success;
    *ready_index = fired_idx;
    return _SEL_EPOLL_BREAK;
  }
  /* A queue eventfd fired: fall through to Phase 3. */
  return _SEL_EPOLL_CONTINUE;
}

/* Blocks until at least one of the n selectables is ready (or timeout_ms
 * elapses), then sets *ready_index.  ccol_select_timed() never performs the
 * receive/send itself, for any selectable type; the caller does so
 * explicitly afterward.  timeout_ms == -1 means wait indefinitely.
 *
 * Locking protocol (prevents deadlock and lost wakeups):
 *
 * Each iteration is three phases:
 *
 *   Phase 1: Per-queue (under each queue's mutex, one at a time):
 *     Read direction: check msg_count > 0 (peek only; nothing is consumed).
 *       If ready, forward the notify to the next read waiter (mirrors the
 *       write-direction cascade below) and mark found.  If not ready,
 *       prepend a waiter node to the queue's sel_read_waiters_head list and
 *       wait regardless of writing_disabled state.
 *     Write direction: check msg_count < max_size && !writing_disabled for
 *       circular_queue; !writing_disabled && msg_count < max_elem_count for
 *       dynamic_queue.  If writable, mark found immediately (nothing is
 *       reserved or consumed).  If not writable, prepend a waiter
 *       node to sel_write_waiters_head (no terminal state; keeps waiting).
 *     fd selectables: registered directly in the epoll set (see below).
 *
 *   Phase 2: Wait:
 *     No fd selectables present: block on sel_cond under sel_mtx.  The
 *       while-loop guards against spurious wakeups and against signals that
 *       fired between Phase 1 and cond_wait.  Producers set the ready flag
 *       under sel_mtx before signalling, so no wakeup can be lost.
 *       If a deadline is set, cond_var_timedwait is used on a
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
 *   Phase 3: Deregister:
 *     Re-acquire each queue's mutex, splice the node out of the correct waiter
 *     list, drain each open eventfd with a non-blocking read (resetting its
 *     counter to 0 for the next iteration), and loop back to Phase 1.
 *
 * Lock ordering: producers take (queue mutex, then sel_mtx). ccol_select takes
 * each queue mutex alone in Phase 1 and Phase 3, and sel_mtx alone in Phase 2
 * (condvar path).  epoll_wait holds no application-level locks.  No two locks
 * are ever held simultaneously, so there is no lock-ordering cycle.
 *
 * Node lifetime: producer notification happens while the producer holds the
 * queue mutex.  Deregistration also requires the queue mutex.  Therefore a
 * node cannot be removed from the list while a producer is traversing it;
 * the heap node cannot vanish mid-traversal.
 *
 * epfd lifecycle: created once per call before the loop.  User fds are added
 * once with EPOLL_CTL_ADD before the loop and kept registered for the entire
 * call; level-triggered semantics ensure a ready fd continues to fire on every
 * epoll_wait until the caller consumes it.  Closing epfd on return removes
 * them automatically; no per-iteration DEL/ADD needed.  Queue eventfds are
 * created per-waiter on the first registration and reused across iterations:
 * deregister_sel_waiter drains them (non-blocking read) instead of closing,
 * so Phase 1 re-links the existing node without any new epoll_ctl or eventfd
 * syscalls. */
ccol_retval_t ccol_select_timed(size_t *ready_index, size_t n,
                                ccol_selectable *selectables, int timeout_ms) {
  ccol_retval_t v = _sel_validate_args(ready_index, n, selectables);
  if (v != ccol_success) return v;

  bool has_fd_sels = false;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == ccol_selectable_fd) {
      has_fd_sels = true;
      break;
    }
  }

  /* One waiter node per selectable.  sel_mtx == NULL means "not currently
   * registered in any queue's list."  efd == -1 means "no eventfd allocated"
   * (condvar-only mode).  Heap-allocated to avoid stack overflow for large n.
   */
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
   * wall-clock adjustments.  Infinite waits ignore the clock attribute so
   * this is safe even when no timeout is used. */
  {
    cond_var_attr_t cond_attr;
    cond_var_attr_init(cond_attr);
    cond_var_attr_setclock(cond_attr, CLOCK_MONOTONIC);
    cond_var_init_ca(sel_cond, cond_attr);
    cond_var_attr_destroy(cond_attr);
  }

  bool has_deadline;
  struct timespec deadline = {0, 0};
  has_deadline = _sel_compute_deadline(timeout_ms, &deadline);

  int epfd = -1;
  if (has_fd_sels) {
    epfd = _sel_setup_epoll(n, selectables);
    if (epfd < 0) {
      free(nodes);
      mutex_destroy(sel_mtx);
      cond_var_destroy(sel_cond);
      return ccol_unexpected_failure;
    }
  }

  ccol_retval_t retval = ccol_success;

  for (;;) {
    /* === Phase 1: scan + register === */
    int found = _sel_phase1_scan_register(n, selectables, nodes, has_fd_sels,
                                          epfd, &sel_mtx, &sel_cond, &ready);
    if (found == -2) goto cleanup_unexpected_failure;
    if (found >= 0) {
      deregister_all_sel_waiters(n, nodes, selectables);
      *ready_index = (size_t)found;
      retval = ccol_success;
      break;
    }

    /* === Phase 2: wait === */
    if (!has_fd_sels) {
      bool timed_out = _sel_wait_condvar(&sel_mtx, &sel_cond, &ready,
                                         has_deadline, &deadline);
      if (timed_out) {
        deregister_all_sel_waiters(n, nodes, selectables);
        retval = ccol_timed_out;
        break;
      }
    } else {
      _sel_epoll_outcome outcome =
          _sel_wait_epoll(epfd, has_deadline, &deadline, n, selectables, nodes,
                          ready_index, &retval);
      if (outcome == _SEL_EPOLL_BREAK) break;
      if (outcome == _SEL_EPOLL_FAILURE) goto cleanup_unexpected_failure;
      /* _SEL_EPOLL_CONTINUE: fall through to Phase 3 */
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

ccol_retval_t ccol_select(size_t *ready_index, size_t n,
                          ccol_selectable *selectables) {
  return ccol_select_timed(ready_index, n, selectables, -1);
}

/* event_loop related section starts here. */

typedef struct event_entry event_entry;

struct event_reg {
  ccol_selectable sel;
  event_handlers_t handlers;
  void *arg;
  _Atomic int refcount; /* 1 while registered; +1 per in-flight callback */
  _Atomic bool removed;
  event_entry *owning_entry;

  /* Which of loop->stripes this registration's entry belongs to. Set once,
   * at the same point owning_entry is set, and never written again.
   * event_loop_modify/event_loop_remove must key their stripe lookup off
   * THIS field, not owning_entry->stripe_idx: a legitimately-held stale
   * reg* (the exact scenario _event_loop_defer_reg_free's contract exists
   * for) can have an owning_entry that has already been freed, since entry
   * and reg have separate, independently-drained deferred-free lists. reg's
   * own deferred-free list is what guarantees reg->stripe_idx itself is
   * always safe to read; entry's is a different, unrelated guarantee that
   * doesn't extend to protecting reads made through a stale reg. */
  size_t stripe_idx;

  /* Caller-visible identity token (event_loop_reg_generation), copied from
   * owning_entry->generation at the same point stripe_idx is copied and for
   * the identical reason: a stale reg* must never read this through
   * owning_entry, which can already be freed independently. Set once, never
   * written again after that copy -- safe to read unconditionally under
   * reg's own deferred-free contract, exactly like stripe_idx. */
  uint64_t generation;

  /* Snapshot of loop->global_gen taken at the moment this reg is pushed
   * onto loop->pending_reg_frees (see _event_loop_defer_reg_free). Not
   * meaningful until removed is true; determines when every reactor
   * thread has definitively finished any batch that could still reference
   * this reg, so freeing it is safe under multi-threaded dispatch. See the
   * large comment above _event_loop_reclaim_pending_frees. */
  size_t defer_gen;

  int bridge_efd; /* -1 for fd selectables; the persistent bridge eventfd
                   * for queue/channel selectables */

  /* Queue/channel selectables only: _notify_waiter (above) unconditionally
   * locks sel_mtx / signals sel_cond before checking efd, so a persistent
   * registration needs its own owned mutex/cond/ready-bool to satisfy that
   * contract, even though the reactor thread never actually
   * cond_var_wait's on wait_cond (only the bridge_efd ping matters
   * here). */
  mutex_t wait_mtx;
  cond_var_t wait_cond;
  bool wait_ready;
  ccol_sel_waiter waiter_node;

  /* Intrusive list of every currently-registered queue-backed reg for this
   * registration's stripe. fd-backed regs don't need this (they're already
   * reachable via that stripe's own fd table); queue selectables have no
   * equivalent table to enumerate them from, and __event_loop_destroy must
   * be able to find and unlink every queue-backed waiter_node from its
   * queue's own list before freeing it. */
  event_reg *loop_list_prev;
  event_reg *loop_list_next;

  /* Linked into loop->pending_reg_frees when its refcount reaches 0 (see
   * _event_loop_defer_reg_free below); never reused once freeing begins. */
  event_reg *pending_free_next;
};

/* What ev.data.ptr always points to for every epoll registration this
 * module owns. For an fd, one entry can be shared by up to two event_regs
 * (read_reg/write_reg), since epoll_ctl keys its interest list by fd, not
 * by (fd, direction) pair; a bare event_reg* cannot be what ev.data.ptr
 * holds directly, or whichever direction's reg was registered last would
 * silently receive every event on that fd, including ones meant for the
 * other direction. Queue/channel selectables never share an entry: each
 * gets its own dedicated bridge eventfd and one-reg entry. */
struct event_entry {
  bool is_fd;
  int fd; /* fd selectables only; also the fd-table key */

  /* Which of loop->stripes this entry belongs to (the stripe whose lock
   * protects as.fd.read_reg/write_reg or as.reg, and whose fd_index/
   * queue_regs_head this entry is filed under). Set once at creation,
   * before the entry is ever published (inserted into a stripe's chmap /
   * epoll_ctl'd), and never written again -- every reader (the dispatch
   * collector via ev->data.ptr) can therefore read it lock-free. Must NOT
   * be re-derived later from entry->as.reg or entry->as.fd.read_reg/
   * write_reg outside a lock: event_loop_remove's queue branch nulls
   * entry->as.reg specifically so a stale, already-fetched epoll batch
   * entry can't dereference a freed reg through it, and re-deriving a
   * stripe key from that exact field at dispatch time would defeat that. */
  size_t stripe_idx;

  /* Per-entry dispatch lock: with more than one reactor thread, epoll's
   * default (non-EPOLLEXCLUSIVE) level-triggered semantics mean two threads
   * genuinely can each receive this same still-ready entry from their own
   * concurrent epoll_wait call (the classic "thundering herd") -- nothing
   * about epoll itself prevents that. Held across the entire dispatch of
   * this entry (both the read_reg/write_reg collection under the stripe
   * lock below AND the callback invocation itself, unlike the stripe lock
   * which is only ever held for collection), this is what actually
   * guarantees a single registration's callback is never invoked
   * concurrently with itself, and -- since both directions on one fd share
   * this same entry -- that a read and a write registration on the same fd
   * never run concurrently with each other either. This is a materially
   * stricter guarantee than facio's own FIO_PR_LOCK_TASK/FIO_PR_LOCK_WRITE
   * (which only serializes on_data against itself and on_ready against
   * itself, not against each other), deliberately, so a caller layering a
   * single shared resource (e.g. one TLS connection object) across both
   * directions of one fd never needs an ad-hoc lock of its own the way this
   * codebase's own chttpclient.c tls_lock had to be added after the fact to
   * compensate for facio's lack of that exact guarantee.
   *
   * NOT the same lock as the stripe lock: the stripe lock protects the
   * registry (read_reg/write_reg slots, the fd_index chmap) against
   * concurrent event_loop_add/_remove/_modify calls from ANY thread; this
   * lock protects one entry's dispatch against concurrent reactor threads.
   * Never held while attempting to acquire a stripe lock from a different
   * entry, and no other code path acquires a stripe lock and then tries to
   * acquire this lock, so no new lock-ordering cycle is introduced. */
  mutex_t dispatch_lock;

  /* Caller-visible identity token (event_loop_reg_generation): minted once,
   * from loop->fd_generation_counter, at entry-creation time (never for an
   * existing entry gaining its second direction) and copied into every
   * reg->generation that ever attaches to this entry. See event_reg's own
   * generation field for why reads through a stale reg must never go
   * through owning_entry->generation instead of reg's own copy. */
  uint64_t generation;

  /* Snapshot of loop->global_gen taken at the moment this entry is pushed
   * onto loop->pending_entry_frees (see _event_loop_defer_entry_free). See
   * the large comment above _event_loop_reclaim_pending_frees for the full
   * multi-threaded reclamation scheme this supports. */
  size_t defer_gen;

  union {
    struct {
      event_reg *read_reg;
      event_reg *write_reg;
    } fd;
    event_reg *reg; /* queue/channel selectables: 1:1, no sharing */
  } as;

  /* Linked into loop->pending_entry_frees when retired (see
   * _event_loop_defer_entry_free below); never reused once an entry is
   * retired, so this doubling as both "live" and "pending free" state is
   * safe. */
  event_entry *pending_free_next;
};

/* One independent (mutex, fd index, queue-reg list) triple. A real fd or a
 * queue/channel registration's private bridge eventfd is always handled by
 * exactly one stripe for its entire lifetime (see _stripe_index_for_fd and
 * the round-robin queue assignment in event_loop_add), so no operation ever
 * needs to hold more than one stripe's lock at once. */
typedef struct event_loop_stripe {
  mutex_t lock;
  chmap fd_index;             /* int fd -> event_entry*; real fds only */
  event_reg *queue_regs_head; /* queue/channel-backed regs in this stripe */
} event_loop_stripe_t;

struct event_loop_s {
  int epfd;
  int shutdown_efd;

  /* One or more background reactor threads, all calling epoll_wait on the
   * same epfd (see event_loop_create_with_mprocs's num_reactor_threads).
   * num_threads == 1 reproduces this module's original single-thread
   * behavior exactly, including timing (see the reclamation scheme below). */
  thread_id_t *threads;
  size_t num_threads;

  /* Guards only shutdown_started/joined/joined_cv (event_loop_shutdown's
   * one-shot leader/follower coordination). Never touches per-fd state --
   * do not confuse with a per-stripe lock; renamed from the original
   * single-lock design's registry_lock specifically to avoid that
   * confusion once the registry itself moved to stripes[]. */
  mutex_t shutdown_lock;
  cond_var_t joined_cv;
  bool shutdown_started;
  bool joined;
  _Atomic bool shutting_down;

  /* event_entry structs retired by event_loop_remove but not yet freed.
   * See _event_loop_defer_entry_free's comment for why a synchronous free
   * there would be a use-after-free. Lock-free Treiber-stack head (push via
   * CAS, drained via a single atomic_exchange) -- loop-wide aggregate
   * state, not per-stripe, since entries from every stripe are threaded
   * onto this one list. */
  _Atomic(event_entry *) pending_entry_frees;

  /* event_reg structs whose refcount reached 0 but are not yet freed. A
   * synchronous free here (the common case: refcount reaches 0 immediately,
   * with no in-flight dispatch) would leave a caller-held event_reg* that
   * still gets passed to event_loop_modify/event_loop_remove (both
   * documented to gracefully return ccol_invalid_args for an
   * already-removed reg, not to be undefined behaviour) pointing at freed
   * memory; see _event_loop_defer_reg_free's comment. Lock-free, same shape
   * as pending_entry_frees. */
  _Atomic(event_reg *) pending_reg_frees;

  /* Multi-threaded deferred-free reclamation (see the large comment above
   * _event_loop_reclaim_pending_frees for the full design). With a single
   * reactor thread, "safe to free" was simply "the next between-batches
   * point"; with N threads, a deferred item must not be freed until every
   * thread has independently proven it has completed whatever batch it was
   * processing at defer time, since any of them could be mid-processing a
   * stale, already-fetched epoll_wait batch that still references it.
   *
   * global_gen is a loop-wide monotonic counter, incremented by whichever
   * reactor thread reaches its own between-batches point (right before
   * calling epoll_wait again). thread_reached_gen[i] records the highest
   * global_gen value thread i has personally observed at one of its own
   * between-batches points -- allocated once, sized to num_threads, at
   * loop creation. A deferred item's own defer_gen field (see event_entry
   * and event_reg) is a snapshot of global_gen taken at defer time; the
   * item is safe to free once defer_gen < every thread_reached_gen[i]
   * (equivalently, defer_gen < the minimum across all threads), since that
   * means every thread has crossed a between-batches point -- and
   * therefore fully finished any batch it might have had in flight -- since
   * the deferral happened. With num_threads == 1 this reduces to exactly
   * the original single-thread timing: the deferring thread's very next
   * between-batches point. */
  _Atomic size_t global_gen;
  _Atomic size_t *thread_reached_gen; /* array of num_threads atomics */

  /* Mints event_loop_reg_generation's caller-visible identity token.
   * Loop-wide, monotonic, starts at 1 (0 is reserved to mean "no reg", see
   * event_loop_reg_generation's own doc comment) -- incremented once per
   * NEW event_entry (not per event_loop_add call: a second direction
   * joining an already-registered fd shares the existing entry's
   * generation, it doesn't mint a new one). */
  _Atomic uint64_t fd_generation_counter;

  size_t max_events_per_wait;

  /* Lock-striped fd/entry registry: num_stripes independent (mutex, chmap,
   * queue-reg list) triples. See event_loop_stripe_t and
   * _stripe_index_for_fd. */
  event_loop_stripe_t *stripes;
  size_t num_stripes;

  /* Round-robin cursor for assigning queue/channel registrations to a
   * stripe (see event_loop_add): unlike a real fd, a queue selectable's
   * entry is never looked up by a second call (no combining), so its
   * stripe assignment has no consistency requirement to satisfy and can be
   * anything deterministic-per-registration -- round-robin is simpler than
   * hashing bridge_efd (which does not even exist yet at the point the
   * stripe must be chosen, since it's only created after the stripe lock
   * is taken) and gives strictly better distribution besides. */
  _Atomic size_t next_queue_stripe;

  _Atomic size_t reg_count;

  ccol_memmgmt_procs_t *m_procs;
};

/* Multiplicative hash (Knuth's constant, in the same spirit as chashmap's
 * own documented Fibonacci hashing for open addressing) reduced mod
 * num_stripes. Plain fd % num_stripes is deliberately avoided: fds are
 * small, kernel-sequential integers, and a plain modulo risks clustering
 * (e.g. an all-even-fd pattern landing in only half the stripes when
 * num_stripes is a power of two). fd selectables only -- queue/channel
 * selectables are assigned via loop->next_queue_stripe instead (see
 * event_loop_add), since their stripe has no consistency requirement to
 * satisfy in the first place. */
static size_t _stripe_index_for_fd(struct event_loop_s *loop, int fd) {
  uint32_t h = (uint32_t)fd * 2654435761u;
  return (size_t)h % loop->num_stripes;
}

/* chmap's separate-chaining chmap_entry storage is __attribute__((packed)),
 * so a stored event_entry* is not guaranteed 8-byte aligned; a direct
 * *(event_entry **)val_pair->ptr cast is UB and can crash at -O3 (the exact
 * alignment hazard already documented for cjson/cyaml's own chmap usage in
 * this codebase). Always read/write the stored pointer via memcpy.
 *
 * These all operate on one stripe's own fd_index/queue_regs_head, passed in
 * directly by the caller (which has already computed the right stripe via
 * _stripe_index_for_fd or the round-robin counter and locked it) -- not on
 * loop as a whole. */
static event_entry *_fd_registry_find(event_loop_stripe_t *stripe, int fd) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  cmap_pair *val_pair = NULL;
  if (chmap_get_elem_ref(stripe->fd_index, &key_pair, &val_pair) !=
      ccol_success) {
    return NULL;
  }
  event_entry *entry;
  memcpy(&entry, val_pair->ptr, sizeof(entry));
  return entry;
}

/* Inserts fd->entry. Caller must have already confirmed fd is not present
 * (via a prior _fd_registry_find returning NULL). */
static bool _fd_registry_insert(event_loop_stripe_t *stripe, int fd,
                                event_entry *entry) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  cmap_pair val_pair = {.ptr = &entry, .size = sizeof(entry)};
  return chmap_insert_elem(stripe->fd_index, &key_pair, &val_pair) ==
         ccol_success;
}

static void _fd_registry_remove(event_loop_stripe_t *stripe, int fd) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  chmap_delete_elem(stripe->fd_index, &key_pair);
}

static void _loop_queue_list_add(event_loop_stripe_t *stripe, event_reg *reg) {
  reg->loop_list_prev = NULL;
  reg->loop_list_next = stripe->queue_regs_head;
  if (stripe->queue_regs_head) stripe->queue_regs_head->loop_list_prev = reg;
  stripe->queue_regs_head = reg;
}

static void _loop_queue_list_remove(event_loop_stripe_t *stripe,
                                    event_reg *reg) {
  if (reg->loop_list_prev)
    reg->loop_list_prev->loop_list_next = reg->loop_list_next;
  else
    stripe->queue_regs_head = reg->loop_list_next;
  if (reg->loop_list_next)
    reg->loop_list_next->loop_list_prev = reg->loop_list_prev;
}

/* Resolves the queue mutex and the correct sel_{read,write}_waiters_head
 * pointer for sel (circq or dynq only; fd selectables never reach here).
 * ccol_selectable_from_chan has already resolved chan selectables down to a
 * concrete circq by the time sel reaches event_loop_add. */
static void _queue_sel_locate(ccol_selectable *sel, mutex_t **out_mtx,
                              ccol_sel_waiter ***out_head) {
  if (sel->type == ccol_selectable_circq) {
    circular_queue *cq = sel->cq;
    *out_mtx = &cq->mutex;
    *out_head = (sel->dir == ccol_select_read) ? &cq->sel_read_waiters_head
                                               : &cq->sel_write_waiters_head;
  } else {
    dynamic_queue *dq = sel->dq;
    *out_mtx = &dq->mutex;
    *out_head = (sel->dir == ccol_select_read) ? &dq->sel_read_waiters_head
                                               : &dq->sel_write_waiters_head;
  }
}

static event_reg *_event_reg_create(struct event_loop_s *loop,
                                    ccol_selectable sel,
                                    event_handlers_t handlers, void *arg) {
  event_reg *reg = _mem_calloc(loop->m_procs, 1, sizeof(event_reg));
  if (!reg) return NULL;
  reg->sel = sel;
  reg->handlers = handlers;
  reg->arg = arg;
  atomic_init(&reg->refcount, 1);
  atomic_init(&reg->removed, false);
  reg->bridge_efd = -1;
  return reg;
}

static void _event_reg_free(struct event_loop_s *loop, event_reg *reg) {
  if (reg->sel.type != ccol_selectable_fd) {
    mutex_destroy(reg->wait_mtx);
    cond_var_destroy(reg->wait_cond);
    if (reg->bridge_efd >= 0) close(reg->bridge_efd);
  }
  _mem_free(loop->m_procs, reg);
}

/* Registers a new fd direction for reg, whose stripe is idx (computed by
 * the caller, event_loop_add, from sel.fd before any lock was taken). If
 * sel.fd already has an event_entry (its other direction is already
 * registered), combines interest via EPOLL_CTL_MOD; otherwise creates a
 * fresh entry via EPOLL_CTL_ADD. Rejects a direction that's already
 * occupied by a different reg with ccol_not_permitted. On success, sets
 * reg->owning_entry and reg->stripe_idx (and entry->stripe_idx, for a new
 * entry). Caller must already hold loop->stripes[idx].lock. */
static ccol_retval_t _event_loop_add_fd(struct event_loop_s *loop, size_t idx,
                                        event_reg *reg) {
  event_loop_stripe_t *stripe = &loop->stripes[idx];
  int fd = reg->sel.fd;
  event_entry *entry = _fd_registry_find(stripe, fd);
  bool new_entry = (entry == NULL);

  if (new_entry) {
    entry = _mem_calloc(loop->m_procs, 1, sizeof(event_entry));
    if (!entry) return ccol_not_enough_memory;
    entry->is_fd = true;
    entry->fd = fd;
    entry->stripe_idx = idx;
    entry->as.fd.read_reg = NULL;
    entry->as.fd.write_reg = NULL;
    mutex_init(entry->dispatch_lock);
    /* Minted once per NEW entry, never for a second direction joining an
     * already-registered fd (that case takes the !new_entry path below and
     * shares the existing entry's generation, correctly reflecting that
     * both directions represent one logical connection). */
    entry->generation = atomic_fetch_add(&loop->fd_generation_counter, 1) + 1;
  }

  event_reg **slot = (reg->sel.dir == ccol_select_read)
                         ? &entry->as.fd.read_reg
                         : &entry->as.fd.write_reg;
  if (*slot != NULL) {
    if (new_entry) {
      mutex_destroy(entry->dispatch_lock);
      _mem_free(loop->m_procs, entry);
    }
    return ccol_not_permitted;
  }
  *slot = reg;

  uint32_t mask = 0;
  if (entry->as.fd.read_reg)
    mask |= (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
  if (entry->as.fd.write_reg) mask |= (EPOLLOUT | EPOLLERR | EPOLLHUP);

  struct epoll_event ev;
  ev.data.ptr = entry;
  ev.events = mask;
  int ctl_op = new_entry ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  if (epoll_ctl(loop->epfd, ctl_op, fd, &ev) < 0) {
    *slot = NULL;
    if (new_entry) {
      mutex_destroy(entry->dispatch_lock);
      _mem_free(loop->m_procs, entry);
    }
    return ccol_unexpected_failure;
  }

  if (new_entry && !_fd_registry_insert(stripe, fd, entry)) {
    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL);
    *slot = NULL;
    mutex_destroy(entry->dispatch_lock);
    _mem_free(loop->m_procs, entry);
    return ccol_not_enough_memory;
  }

  reg->owning_entry = entry;
  reg->stripe_idx = idx;
  reg->generation = entry->generation;
  return ccol_success;
}

/* Sets up a queue-backed registration: allocates the dedicated bridge
 * eventfd, registers it with the loop's persistent epoll instance, and
 * links reg's embedded waiter_node into the queue's own waiter list,
 * permanently (unlike ccol_select's transient per-call nodes), so the
 * queue's existing notify_one_sel_waiter/notify_all_sel_waiters (already
 * called from circq_send_zc/recv_zc, dynmq_send_zc/recv_zc, and the
 * enable_sending/disable_sending functions) wakes this registration too. */
static ccol_retval_t _event_loop_add_queue(struct event_loop_s *loop,
                                           event_entry *entry, event_reg *reg) {
  mutex_init(reg->wait_mtx);
  cond_var_init(reg->wait_cond);
  reg->wait_ready = false;

  reg->bridge_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (reg->bridge_efd < 0) {
    mutex_destroy(reg->wait_mtx);
    cond_var_destroy(reg->wait_cond);
    return ccol_unexpected_failure;
  }
  reg->waiter_node.efd = reg->bridge_efd;

  struct epoll_event ev;
  ev.data.ptr = entry;
  ev.events = EPOLLIN;
  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, reg->bridge_efd, &ev) < 0) {
    close(reg->bridge_efd);
    reg->bridge_efd = -1;
    mutex_destroy(reg->wait_mtx);
    cond_var_destroy(reg->wait_cond);
    return ccol_unexpected_failure;
  }

  mutex_t *q_mtx;
  ccol_sel_waiter **q_head;
  _queue_sel_locate(&reg->sel, &q_mtx, &q_head);

  mutex_lock(*q_mtx);
  /* Single-element-array call convention: _sel_link_waiter indexes into
   * nodes[i] because ccol_select_timed always has a real caller-owned
   * array; event_loop has exactly one standalone waiter_node per
   * registration, and nodes[0] with nodes = &reg->waiter_node is just
   * reg->waiter_node. */
  _sel_link_waiter(0, &reg->waiter_node, &reg->wait_mtx, &reg->wait_cond,
                   &reg->wait_ready, q_head);
  /* Unlike a real fd (where epoll_ctl(ADD) against an already-readable
   * kernel object is picked up by the very next epoll_wait, since epoll
   * tracks the resource's live state, not just edge transitions), this
   * bridge eventfd only rings on a *future* notify_one_sel_waiter call.  A
   * message already sitting in the queue before this registration existed
   * would otherwise be missed entirely until the next send.  Self-trigger
   * here, still under q_mtx so the check is consistent with the link above,
   * if the queue is already in the target state; the reactor thread's own
   * next epoll_wait then picks it up and dispatches through the completely
   * standard drain-then-try_recv path, so callbacks still only ever run
   * from there. */
  bool already_ready =
      (reg->sel.type == ccol_selectable_circq)
          ? (reg->sel.dir == ccol_select_read
                 ? reg->sel.cq->msg_count > 0
                 : reg->sel.cq->msg_count < reg->sel.cq->max_size &&
                       !reg->sel.cq->writing_disabled)
          : (reg->sel.dir == ccol_select_read
                 ? reg->sel.dq->msg_count > 0
                 : !reg->sel.dq->writing_disabled &&
                       reg->sel.dq->msg_count < max_elem_count);
  mutex_unlock(*q_mtx);

  if (already_ready) {
    uint64_t one = 1;
    (void)write(reg->bridge_efd, &one, sizeof(one));
  }

  return ccol_success;
}

/* Validates a ccol_selectable for event_loop_add. */
static ccol_retval_t _event_loop_validate_add_args(event_loop loop,
                                                   ccol_selectable *sel) {
  if (!loop) return ccol_invalid_args;
  if (sel->dir != ccol_select_read && sel->dir != ccol_select_write)
    return ccol_invalid_args;

  if (sel->type == ccol_selectable_fd) {
    if (sel->fd < 0) return ccol_invalid_args;
  } else if (sel->type == ccol_selectable_circq) {
    if (!sel->cq) return ccol_invalid_args;
  } else if (sel->type == ccol_selectable_dynq) {
    if (!sel->dq) return ccol_invalid_args;
  } else {
    return ccol_invalid_args;
  }
  return ccol_success;
}

event_reg *event_loop_add(event_loop loop, ccol_selectable sel,
                          event_handlers_t handlers, void *arg,
                          char **err_str) {
  ccol_retval_t validate = _event_loop_validate_add_args(loop, &sel);
  if (validate != ccol_success) {
    if (err_str) *err_str = CCOL_ERR_STR("Invalid arguments to event_loop_add");
    return NULL;
  }

  event_reg *reg = _event_reg_create(loop, sel, handlers, arg);
  if (!reg) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate memory for event_reg");
    return NULL;
  }

  /* Stripe index is computed before any lock is taken, from data already
   * available: sel.fd for fd selectables (a pure function of the fd, so a
   * second event_loop_add call for the fd's other direction independently
   * recomputes the same stripe and finds the existing entry via that
   * stripe's own chmap), or loop->next_queue_stripe's round-robin cursor
   * for queue/channel selectables (no such consistency requirement exists
   * for those -- see event_loop_stripe_t's own comment). */
  size_t idx =
      (sel.type == ccol_selectable_fd)
          ? _stripe_index_for_fd(loop, sel.fd)
          : (atomic_fetch_add(&loop->next_queue_stripe, 1) % loop->num_stripes);
  event_loop_stripe_t *stripe = &loop->stripes[idx];

  mutex_lock(stripe->lock);

  ccol_retval_t rv;
  if (sel.type == ccol_selectable_fd) {
    rv = _event_loop_add_fd(loop, idx, reg);
  } else {
    event_entry *entry = _mem_calloc(loop->m_procs, 1, sizeof(event_entry));
    if (!entry) {
      rv = ccol_not_enough_memory;
    } else {
      entry->is_fd = false;
      entry->fd = -1;
      entry->stripe_idx = idx;
      entry->as.reg = reg;
      mutex_init(entry->dispatch_lock);
      /* Queue/channel selectables never share an entry (1:1, no combining),
       * so every event_loop_add call here mints a fresh generation --
       * unlike the fd path, there is no "second direction joins the
       * existing entry" case to special-case. */
      entry->generation =
          atomic_fetch_add(&loop->fd_generation_counter, 1) + 1;
      rv = _event_loop_add_queue(loop, entry, reg);
      if (rv == ccol_success) {
        reg->owning_entry = entry;
        reg->stripe_idx = idx;
        reg->generation = entry->generation;
        _loop_queue_list_add(stripe, reg);
      } else {
        mutex_destroy(entry->dispatch_lock);
        _mem_free(loop->m_procs, entry);
      }
    }
  }

  if (rv == ccol_success) atomic_fetch_add(&loop->reg_count, 1);

  mutex_unlock(stripe->lock);

  if (rv != ccol_success) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to register selectable with event_loop");
    _event_reg_free(loop, reg);
    return NULL;
  }

  return reg;
}

uint64_t event_loop_reg_generation(const event_reg *reg) {
  if (!reg) return 0;
  return reg->generation;
}

/* Deliberately does not also take entry->dispatch_lock: doing so would
 * require reading reg->owning_entry before the stripe lock has confirmed
 * reg->removed is false, which is exactly the unsafe read pattern the
 * owning_entry doc comment (see event_reg's own stripe_idx field) warns
 * against -- entry->dispatch_lock is only ever acquired after that stripe-
 * lock-protected confirmation, in _event_loop_handle_event, never before
 * it. This function's write to reg->sel.dir below can therefore genuinely
 * run concurrently with an in-flight callback's use of *sel (a real,
 * pre-existing race, not one this module's own dispatch_lock introduces or
 * closes) -- see _dispatch_item.sel_snapshot's own comment for how that's
 * made safe instead: the dispatch path snapshots reg->sel under the stripe
 * lock at collection time rather than handing callbacks a live pointer
 * into reg->sel, so this function's write here (also under the stripe
 * lock) can never race a callback's read. */
ccol_retval_t event_loop_modify(event_loop loop, event_reg *reg,
                                ccol_select_dir new_dir) {
  if (!loop || !reg) return ccol_invalid_args;
  if (new_dir != ccol_select_read && new_dir != ccol_select_write)
    return ccol_invalid_args;
  if (reg->sel.type != ccol_selectable_fd) return ccol_invalid_args;

  /* reg->stripe_idx, not reg->owning_entry->stripe_idx: safe to read
   * unconditionally, for any reg* the caller legitimately holds, removed or
   * not, because it's protected by reg's OWN deferred-free contract.
   * owning_entry has a separate, independent deferred-free list, and a
   * stale reg's owning_entry may already have been freed -- see
   * event_reg.stripe_idx's own doc comment. */
  event_loop_stripe_t *stripe = &loop->stripes[reg->stripe_idx];
  mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    mutex_unlock(stripe->lock);
    return ccol_invalid_args;
  }

  if (reg->sel.dir == new_dir) {
    mutex_unlock(stripe->lock);
    return ccol_success;
  }

  /* removed was just confirmed false above, under this exact stripe's lock
   * (the only lock under which it can become true), so owning_entry is
   * guaranteed not yet freed here. */
  event_entry *entry = reg->owning_entry;
  event_reg **target_slot = (new_dir == ccol_select_read)
                                ? &entry->as.fd.read_reg
                                : &entry->as.fd.write_reg;
  if (*target_slot != NULL) {
    mutex_unlock(stripe->lock);
    return ccol_not_permitted;
  }

  event_reg **current_slot = (reg->sel.dir == ccol_select_read)
                                 ? &entry->as.fd.read_reg
                                 : &entry->as.fd.write_reg;
  *current_slot = NULL;
  *target_slot = reg;
  reg->sel.dir = new_dir;

  uint32_t mask = 0;
  if (entry->as.fd.read_reg)
    mask |= (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
  if (entry->as.fd.write_reg) mask |= (EPOLLOUT | EPOLLERR | EPOLLHUP);
  struct epoll_event ev;
  ev.data.ptr = entry;
  ev.events = mask;
  epoll_ctl(loop->epfd, EPOLL_CTL_MOD, entry->fd, &ev);

  mutex_unlock(stripe->lock);
  return ccol_success;
}

/* event_entry cannot be freed synchronously from event_loop_remove, even
 * though epoll_ctl(DEL)/entry-slot-clearing already prevents any FUTURE
 * epoll_wait call from returning a new event for it. epoll_wait can return
 * a batch of several ready events in one call, which a reactor thread then
 * processes one at a time; if this entry's event is sitting at some later
 * index in a batch already fetched (fetched before this remove() call,
 * sitting in that thread's local stack array), freeing the entry here races
 * that not-yet-processed index against event_loop_remove, a genuine
 * use-after-free reproduced via a real SIGSEGV during this feature's own
 * test development (gdb backtrace on the resulting core pinned it to a
 * stale event_entry* read after a concurrent remove()). epoll_ctl(DEL) has
 * no way to retroactively invalidate an event already copied out of the
 * kernel into userspace.
 *
 * With a single reactor thread, deferring the actual free to a point where
 * that one thread can prove no batch could still reference this entry
 * (between finishing one epoll_wait batch and starting the next) closed the
 * race completely. With more than one reactor thread, "the reactor thread's
 * own next batch boundary" no longer means anything -- ANY of the N threads
 * could be mid-processing a stale, already-fetched batch that references
 * this entry, independent of which thread happens to reach its own next
 * batch boundary first. See the large comment above
 * _event_loop_reclaim_pending_frees for the epoch-based scheme this module
 * uses to generalize the same guarantee to N threads; this function only
 * pushes the node with its defer_gen snapshot stamped, it does not decide
 * when freeing is actually safe.
 *
 * Lock-free Treiber-stack push (loop->pending_entry_frees is loop-wide
 * aggregate state, not per-stripe -- entries from every stripe are threaded
 * onto this one list, so a single stripe lock couldn't protect it anyway).
 * No ABA hazard: a node is only ever popped as part of claiming the WHOLE
 * list at once via one atomic_exchange (see
 * _event_loop_reclaim_pending_frees), never popped and freed one node at a
 * time while the list is still shared. */
static void _event_loop_push_entry_free_node(struct event_loop_s *loop,
                                             event_entry *entry) {
  event_entry *old_head = atomic_load(&loop->pending_entry_frees);
  do {
    entry->pending_free_next = old_head;
  } while (!atomic_compare_exchange_weak(&loop->pending_entry_frees, &old_head,
                                         entry));
}

static void _event_loop_defer_entry_free(struct event_loop_s *loop,
                                         event_entry *entry) {
  entry->defer_gen = atomic_load(&loop->global_gen);
  _event_loop_push_entry_free_node(loop, entry);
}

/* event_reg cannot be freed synchronously either, for a related but
 * distinct reason from _event_loop_defer_entry_free's: event_loop_modify
 * and event_loop_remove are both documented to gracefully return
 * ccol_invalid_args (not invoke undefined behaviour) when called on a reg
 * that was already removed. Freeing reg the instant its refcount reaches
 * 0 (the common case, whenever no dispatch happens to be in flight for it)
 * means a caller that calls event_loop_remove and then passes that same
 * (now-dangling) pointer to event_loop_modify hits exactly the use-after-
 * free the documented contract promises can't happen; caught directly by
 * valgrind on this feature's own test for that exact scenario. Deferring
 * the free the same way as entries keeps the memory (and its `removed`,
 * `stripe_idx`, and `generation` fields) valid for that later access to
 * read safely. Same shape as _event_loop_push_entry_free_node/
 * _event_loop_defer_entry_free, including the same multi-threaded
 * generalization via defer_gen. */
static void _event_loop_push_reg_free_node(struct event_loop_s *loop,
                                           event_reg *reg) {
  event_reg *old_head = atomic_load(&loop->pending_reg_frees);
  do {
    reg->pending_free_next = old_head;
  } while (
      !atomic_compare_exchange_weak(&loop->pending_reg_frees, &old_head, reg));
}

static void _event_loop_defer_reg_free(struct event_loop_s *loop,
                                       event_reg *reg) {
  reg->defer_gen = atomic_load(&loop->global_gen);
  _event_loop_push_reg_free_node(loop, reg);
}

/* Returns the minimum of thread_reached_gen[] across every reactor thread --
 * the highest defer_gen value for which we can be certain EVERY thread has
 * crossed at least one of its own between-batches points since that
 * generation was current, and therefore fully finished any batch it might
 * have had in flight then. See the struct event_loop_s field comments for
 * the full design. */
static size_t _event_loop_min_reached_gen(struct event_loop_s *loop) {
  size_t min = atomic_load(&loop->thread_reached_gen[0]);
  for (size_t i = 1; i < loop->num_threads; i++) {
    size_t g = atomic_load(&loop->thread_reached_gen[i]);
    if (g < min) min = g;
  }
  return min;
}

/* Reclaims every entry/reg deferred by _event_loop_defer_entry_free /
 * _event_loop_defer_reg_free that is now provably safe to free, and
 * otherwise leaves it pending for a later attempt.
 *
 * With a single reactor thread, "safe to free" used to just mean "between
 * batches" -- the one thread calling this function was, by construction,
 * the only thread that could ever have referenced a deferred item, and it
 * had just finished its own previous batch. With N reactor threads all
 * calling epoll_wait on the same epfd, that reasoning breaks down: epoll's
 * default (non-EPOLLEXCLUSIVE) level-triggered semantics let the same ready
 * entry be handed to more than one thread's concurrent epoll_wait call (the
 * same "thundering herd" property that motivates the per-entry
 * dispatch_lock), so ANY of the N threads could independently be
 * mid-processing a stale batch that references a just-removed item,
 * regardless of which thread happens to call this function right now.
 *
 * This function is called by every reactor thread at its own
 * between-batches point (see _event_loop_thread_fn), after that thread has
 * already published its own advanced epoch via thread_reached_gen. An
 * item is eligible once its defer_gen snapshot is older than every thread's
 * currently-published epoch (_event_loop_min_reached_gen): that means every
 * thread has crossed its own between-batches point at least once since the
 * item was deferred, so none of them can still be mid-processing whatever
 * batch (if any) it had in flight at defer time -- and since the item was
 * already removed from the registry (epoll_ctl(DEL) or slot-clearing)
 * before it was ever deferred, no thread's FUTURE batch can reference it
 * either. With num_threads == 1 this reduces to exactly the original
 * timing: an item becomes eligible at the deferring thread's very next
 * between-batches point.
 *
 * Lock-free: each list's entire contents are claimed in one
 * atomic_exchange, partitioned outside of any lock into "free now" and
 * "not yet eligible", and anything not yet eligible is individually pushed
 * back via the same CAS-based push the original defer call used -- WITHOUT
 * re-stamping defer_gen, since these items' original snapshot is what lets
 * them make forward progress; re-stamping would reset their eligibility
 * clock every time a reclaim attempt finds them still-pending and could
 * starve them indefinitely under sustained load. Also used, in its
 * unconditional form (see _event_loop_free_all_pending), by
 * __event_loop_destroy after every reactor thread has been joined, where no
 * epoch check is needed at all since no thread can reference anything any
 * more. */
static void _event_loop_reclaim_pending_frees(struct event_loop_s *loop) {
  size_t min_reached = _event_loop_min_reached_gen(loop);

  event_entry *e = atomic_exchange(&loop->pending_entry_frees, NULL);
  event_entry *e_keep = NULL;
  while (e) {
    event_entry *next = e->pending_free_next;
    if (e->defer_gen < min_reached) {
      mutex_destroy(e->dispatch_lock);
      _mem_free(loop->m_procs, e);
    } else {
      e->pending_free_next = e_keep;
      e_keep = e;
    }
    e = next;
  }
  while (e_keep) {
    event_entry *next = e_keep->pending_free_next;
    _event_loop_push_entry_free_node(loop, e_keep);
    e_keep = next;
  }

  event_reg *r = atomic_exchange(&loop->pending_reg_frees, NULL);
  event_reg *r_keep = NULL;
  while (r) {
    event_reg *next = r->pending_free_next;
    if (r->defer_gen < min_reached) {
      _event_reg_free(loop, r);
    } else {
      r->pending_free_next = r_keep;
      r_keep = r;
    }
    r = next;
  }
  while (r_keep) {
    event_reg *next = r_keep->pending_free_next;
    _event_loop_push_reg_free_node(loop, r_keep);
    r_keep = next;
  }
}

/* Unconditional variant for use after every reactor thread has already been
 * joined (event_loop_shutdown has returned): no thread exists any more to
 * reference anything, so every remaining deferred item is safe to free
 * regardless of its defer_gen. Frees whatever _event_loop_reclaim_pending_frees
 * left pending from its very last, pre-join invocation. */
static void _event_loop_free_all_pending(struct event_loop_s *loop) {
  event_entry *e = atomic_exchange(&loop->pending_entry_frees, NULL);
  while (e) {
    event_entry *next = e->pending_free_next;
    mutex_destroy(e->dispatch_lock);
    _mem_free(loop->m_procs, e);
    e = next;
  }
  event_reg *r = atomic_exchange(&loop->pending_reg_frees, NULL);
  while (r) {
    event_reg *next = r->pending_free_next;
    _event_reg_free(loop, r);
    r = next;
  }
}

ccol_retval_t event_loop_remove(event_loop loop, event_reg *reg) {
  if (!loop || !reg) return ccol_invalid_args;

  /* reg->stripe_idx, not reg->owning_entry->stripe_idx -- see
   * event_loop_modify's identical comment and event_reg.stripe_idx's own
   * doc comment for why. */
  event_loop_stripe_t *stripe = &loop->stripes[reg->stripe_idx];
  mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    mutex_unlock(stripe->lock);
    return ccol_success;
  }

  /* removed was just confirmed false above, under this exact stripe's lock,
   * so owning_entry is guaranteed not yet freed here. */
  event_entry *entry = reg->owning_entry;

  if (reg->sel.type == ccol_selectable_fd) {
    event_reg **slot = (reg->sel.dir == ccol_select_read)
                           ? &entry->as.fd.read_reg
                           : &entry->as.fd.write_reg;
    *slot = NULL;
    if (entry->as.fd.read_reg == NULL && entry->as.fd.write_reg == NULL) {
      epoll_ctl(loop->epfd, EPOLL_CTL_DEL, entry->fd, NULL);
      _fd_registry_remove(stripe, entry->fd);
      _event_loop_defer_entry_free(loop, entry);
    } else {
      uint32_t mask = 0;
      if (entry->as.fd.read_reg)
        mask |= (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
      if (entry->as.fd.write_reg) mask |= (EPOLLOUT | EPOLLERR | EPOLLHUP);
      struct epoll_event ev;
      ev.data.ptr = entry;
      ev.events = mask;
      epoll_ctl(loop->epfd, EPOLL_CTL_MOD, entry->fd, &ev);
    }
  } else {
    mutex_t *q_mtx;
    ccol_sel_waiter **q_head;
    _queue_sel_locate(&reg->sel, &q_mtx, &q_head);
    _sel_unlink_waiter(&reg->waiter_node, q_head, q_mtx);
    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, reg->bridge_efd, NULL);
    _loop_queue_list_remove(stripe, reg);
    /* entry itself is deferred (safe, still-valid memory) below, but its
     * as.reg field must be nulled HERE, under the lock, before that; a
     * stale batch entry could otherwise read entry->as.reg after reg is
     * deferred-freed and get a still-dangling-looking pointer into the
     * pending-free list rather than a clean NULL. The fd branch above
     * already does the equivalent via *slot = NULL. */
    entry->as.reg = NULL;
    _event_loop_defer_entry_free(loop, entry);
  }

  atomic_store(&reg->removed, true);

  mutex_unlock(stripe->lock);

  atomic_fetch_sub(&loop->reg_count, 1);

  int prev = atomic_fetch_sub(&reg->refcount, 1);
  /* Deferred, not freed here directly; see _event_loop_defer_reg_free's
   * comment: a caller-held reg* may still be passed to event_loop_modify or
   * event_loop_remove again after this call returns, and both are
   * documented to read reg->removed safely in that case. */
  if (prev == 1) _event_loop_defer_reg_free(loop, reg);

  return ccol_success;
}

size_t event_loop_reg_count(event_loop loop) {
  if (!loop) return ccol_invalid_size;
  return atomic_load(&loop->reg_count);
}

/* One reg collected for dispatch under the entry's stripe lock, acted on
 * after that lock is released. */
typedef struct _dispatch_item {
  event_reg *reg;
  bool is_error;
  bool is_readable;
  bool is_writable;
  /* Snapshot of reg->sel taken under the stripe lock at collection time
   * (see _event_loop_handle_event), not a live pointer into reg->sel
   * itself. Pre-existing bug, not introduced by multi-threaded dispatch:
   * event_loop_modify updates reg->sel.dir under the stripe lock only (see
   * its own comment for why it cannot also take entry->dispatch_lock
   * without a lock-ordering conflict against the safe-owning_entry-read
   * pattern), while the dispatch path used to hand callbacks a live
   * `&reg->sel` pointer *after* releasing the stripe lock -- a genuine,
   * unsynchronized concurrent read/write on reg->sel.dir between a
   * callback in flight and a concurrent event_loop_modify call from any
   * other thread, always possible (even with a single reactor thread,
   * since event_loop_modify is documented callable from any thread
   * concurrently with dispatch), just never actually caught until
   * ThreadSanitizer was run against this module for the first time during
   * this feature's own development. A snapshot copied once, under the
   * stripe lock, alongside the rest of collection, closes it: the
   * callback observes a self-consistent, well-defined "as of collection
   * time" selectable, and event_readable_fn/event_writable_fn's own
   * documented contract ("sel->dir reflects the registration's current
   * direction, which may have changed via event_loop_modify") is still
   * satisfied -- collection re-reads reg->sel fresh every single dispatch,
   * so an event_loop_modify call that completed before this collection is
   * still correctly observed; only a modify running fully concurrently
   * with an in-flight callback (previously a data race with an undefined
   * outcome) now deterministically resolves to whichever direction was
   * current at collection time. */
  ccol_selectable sel_snapshot;
} _dispatch_item;

static void _event_loop_run_callback(event_loop loop, _dispatch_item *item) {
  event_reg *reg = item->reg;
  if (item->is_error) {
    if (reg->handlers.on_error)
      reg->handlers.on_error(loop, &item->sel_snapshot, reg->arg);
  } else if (item->is_readable) {
    /* Readiness only, for every selectable type: the reactor never performs
     * the receive itself, the callback does (see event_readable_fn's
     * documentation). */
    if (reg->handlers.on_readable)
      reg->handlers.on_readable(loop, &item->sel_snapshot, reg->arg);
  } else if (item->is_writable) {
    if (reg->handlers.on_writable)
      reg->handlers.on_writable(loop, &item->sel_snapshot, reg->arg);
  }
}

/* Decrements reg's refcount after its callback (if any) has returned;
 * defers it for freeing if this was the last reference and it had already
 * been removed (see _event_loop_defer_reg_free's comment for why this
 * can't be a synchronous free here). refcount is _Atomic and
 * _event_loop_defer_reg_free is lock-free, so no lock is needed here at
 * all -- not even a stripe lock, since nothing here touches entry state. */
static void _event_loop_release_after_dispatch(struct event_loop_s *loop,
                                               event_reg *reg) {
  int prev = atomic_fetch_sub(&reg->refcount, 1);
  if (prev == 1 && atomic_load(&reg->removed)) {
    _event_loop_defer_reg_free(loop, reg);
  }
}

/* Called once per epoll_event returned by epoll_wait, on whichever reactor
 * thread's epoll_wait call happened to return it. Collects live regs to
 * dispatch under entry->stripe_idx's own stripe lock (incrementing each
 * one's refcount so it can't be freed while its callback runs), then
 * releases that lock and runs callbacks unlocked; never holding a stripe
 * lock across a user callback.
 *
 * Held across the WHOLE function, not just collection: entry->dispatch_lock.
 * With more than one reactor thread, epoll's default level-triggered
 * semantics mean this exact entry can genuinely be handed to more than one
 * thread's concurrent epoll_wait call (the same still-ready fd reported to
 * each), so without this lock two threads could both pass the collection
 * step below for the SAME reg and both invoke its callback concurrently --
 * a real double-dispatch, not a memory-safety bug (that's what the
 * generation/defer_gen scheme guards against) but a serialization bug this
 * lock exists specifically to close. Because dispatch_lock lives on the
 * entry (shared by both directions), this also gives read_reg and write_reg
 * callbacks on the same fd mutual exclusion against each other, not just
 * self-exclusion -- see the entry's own dispatch_lock field comment for why
 * that's a deliberately stricter guarantee than facio's equivalent. */
static void _event_loop_handle_event(struct event_loop_s *loop,
                                     struct epoll_event *ev) {
  event_entry *entry = (event_entry *)ev->data.ptr;

  if (entry == NULL) {
    /* The shutdown-eventfd's ev.data.ptr is left NULL; it exists purely to
     * interrupt epoll_wait, nothing to dispatch. Deliberately NOT drained
     * via read() here -- with more than one reactor thread, an earlier
     * version of this function did drain it, and that was a real,
     * reproduced deadlock: whichever thread happened to process this event
     * FIRST reset the eventfd's counter to 0, and any other thread still
     * blocked in its own epoll_wait call at that moment (with timeout=-1)
     * then had nothing left to observe as ready and never returned,
     * leaving event_loop_shutdown's join loop hung on it forever. Leaving
     * the counter non-zero and never draining it means every thread's
     * epoll_wait call -- whichever order they happen to run in, already
     * blocked or not yet called -- keeps seeing this fd as ready
     * (level-triggered) for as long as the process lives, which is exactly
     * what's needed here: shutdown is one-way, so there is no future point
     * where this fd's readiness would need to be revoked. Every thread
     * still reaches the top-of-loop shutting_down check immediately after,
     * so no thread spins on this indefinitely either. */
    return;
  }

  mutex_lock(entry->dispatch_lock);

  _dispatch_item items[2];
  size_t n_items = 0;

  /* entry->stripe_idx is written once, before this entry is ever published
   * (inserted into a stripe's chmap / epoll_ctl'd), and never again -- safe
   * to read here with no lock held yet. */
  event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];
  mutex_lock(stripe->lock);

  if (entry->is_fd) {
    bool is_err = (ev->events & (EPOLLERR | EPOLLHUP)) != 0;
    bool is_in = (ev->events & (EPOLLIN | EPOLLRDHUP)) != 0;
    bool is_out = (ev->events & EPOLLOUT) != 0;

    event_reg *r = entry->as.fd.read_reg;
    if (r && !atomic_load(&r->removed) && (is_err || is_in)) {
      atomic_fetch_add(&r->refcount, 1);
      items[n_items].reg = r;
      /* EPOLLIN/EPOLLRDHUP and EPOLLERR/EPOLLHUP are not mutually
       * exclusive: the kernel legitimately reports both together on the
       * SAME event when a peer writes data and then immediately closes
       * (or resets) the connection -- the bytes are genuinely sitting in
       * the socket's receive buffer, readable, even though the peer is
       * also already gone. Unconditionally prioritising is_err here (as an
       * earlier version of this function did) silently discarded that
       * already-arrived data by dispatching on_error instead of
       * on_readable, with nothing left for the caller to ever read it back
       * out of; a real, reproduced bug (caught via chttpclient's Tier 2
       * unix-domain-socket tests, whose near-zero-latency round trip makes
       * a peer's write-then-close race this fd's own read-side epoll_wait
       * far more often than TCP's added latency ever did, though the same
       * race is possible over TCP too).
       *
       * The fix is conditioned on has_reader, not unconditional: an
       * error-only registration (on_readable == NULL, on_error set --
       * exactly how a caller signals "notify me this fd died, I have no
       * interest in reading it") must still see is_error on a bare hangup
       * with no reader to hand the data to. EPOLLHUP alone (no data ever
       * written, a pure close) also sets EPOLLIN on Linux (reading it
       * would return EOF, which is itself a form of read-readiness), so
       * without this guard such a registration would never be notified at
       * all: _event_loop_run_callback's on_readable branch is a silent
       * no-op when the handler pointer is NULL, unlike on_error. Confirmed
       * via a real regression this exact scenario caused in this module's
       * own existing fd_on_error_fires_for_both_directions test, which
       * registers precisely this on_readable=NULL/on_error-only shape. */
      bool has_reader = (r->handlers.on_readable != NULL);
      items[n_items].is_readable = is_in && has_reader;
      items[n_items].is_error = is_err && !items[n_items].is_readable;
      items[n_items].is_writable = false;
      items[n_items].sel_snapshot = r->sel;
      n_items++;
    }
    event_reg *w = entry->as.fd.write_reg;
    if (w && !atomic_load(&w->removed) && (is_err || is_out)) {
      atomic_fetch_add(&w->refcount, 1);
      items[n_items].reg = w;
      items[n_items].is_error = is_err;
      items[n_items].is_readable = false;
      items[n_items].is_writable = !is_err;
      items[n_items].sel_snapshot = w->sel;
      n_items++;
    }
  } else {
    /* Queue entry: drain the bridge eventfd here, under this stripe's lock,
     * so a racing event_loop_remove (which also takes this exact entry's
     * stripe lock before touching reg->bridge_efd) cannot read()/close() it
     * concurrently. entry->as.reg can legitimately be NULL here:
     * event_loop_remove nulls it (under this same stripe lock) before
     * deferring entry's own free, so a stale batch entry reaching this
     * point after a concurrent removal must be treated as nothing-to-do
     * rather than dereferenced. */
    event_reg *r = entry->as.reg;
    if (r) {
      uint64_t val;
      (void)read(r->bridge_efd, &val, sizeof(val));
    }
    if (r && !atomic_load(&r->removed)) {
      atomic_fetch_add(&r->refcount, 1);
      items[n_items].reg = r;
      items[n_items].is_error = false;
      items[n_items].is_readable = (r->sel.dir == ccol_select_read);
      items[n_items].is_writable = (r->sel.dir == ccol_select_write);
      items[n_items].sel_snapshot = r->sel;
      n_items++;
    }
  }

  mutex_unlock(stripe->lock);

  for (size_t i = 0; i < n_items; i++) {
    _event_loop_run_callback(loop, &items[i]);
    _event_loop_release_after_dispatch(loop, items[i].reg);
  }

  mutex_unlock(entry->dispatch_lock);
}

/* One per spawned reactor thread; freed by the thread itself right before
 * it returns (event_loop_shutdown/destroy only need the thread_id_t handles
 * in loop->threads to join, not this context). */
typedef struct event_loop_thread_ctx {
  struct event_loop_s *loop;
  size_t thread_index;
} event_loop_thread_ctx;

static void *_event_loop_thread_fn(void *arg) {
  event_loop_thread_ctx *ctx = (event_loop_thread_ctx *)arg;
  struct event_loop_s *loop = ctx->loop;
  size_t my_idx = ctx->thread_index;
  _mem_free(loop->m_procs, ctx);

  struct epoll_event *events = _mem_alloc(
      loop->m_procs, loop->max_events_per_wait * sizeof(struct epoll_event));
  if (!events) {
    /* Extremely unlikely (small, fixed-size allocation); nothing safe to do
     * except exit; event_loop_shutdown will still join this thread
     * cleanly, just with zero events ever dispatched by it. */
    return NULL;
  }

  for (;;) {
    if (atomic_load(&loop->shutting_down)) break;

    /* Between batches: this thread has fully iterated over its previous
     * batch (if any) by this point. Publish that fact by advancing this
     * thread's own epoch past the current global_gen, then attempt to
     * reclaim whatever deferred entries/regs that advance (combined with
     * every other thread's own most recent publish) now proves safe to
     * free. See the large comment above _event_loop_reclaim_pending_frees
     * for why a single "between batches" check is no longer sufficient
     * with more than one reactor thread, and struct event_loop_s's own
     * field comments for the full epoch design. With num_threads == 1 this
     * reduces to exactly the original single-thread timing. */
    size_t new_gen = atomic_fetch_add(&loop->global_gen, 1) + 1;
    atomic_store(&loop->thread_reached_gen[my_idx], new_gen);
    _event_loop_reclaim_pending_frees(loop);

    int n = epoll_wait(loop->epfd, events, (int)loop->max_events_per_wait, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < n; i++) {
      _event_loop_handle_event(loop, &events[i]);
    }
  }

  _mem_free(loop->m_procs, events);
  return NULL;
}

/* Destroys the first `created` stripes of loop->stripes (mutex + chmap each)
 * and frees the array itself, using the raw mmgmt_procs parameter -- not
 * loop->m_procs -- to match every other _mem_free call site in
 * event_loop_create_with_mprocs, including the ones that run before
 * loop->m_procs is even populated. Used only for rollback on a
 * creation-time failure; __event_loop_destroy's own stripe teardown does
 * more work (freeing live entries first) and is not built on this. */
static void _destroy_stripes(struct event_loop_s *loop,
                             ccol_memmgmt_procs_t *mmgmt_procs,
                             size_t created) {
  for (size_t i = 0; i < created; i++) {
    chmap_destroy(loop->stripes[i].fd_index);
    mutex_destroy(loop->stripes[i].lock);
  }
  _mem_free(mmgmt_procs, loop->stripes);
}

event_loop event_loop_create_with_mprocs(size_t max_events_per_wait,
                                         size_t num_lock_stripes,
                                         size_t num_reactor_threads,
                                         ccol_memmgmt_procs_t *mmgmt_procs,
                                         char **err_str) {
  if (max_events_per_wait == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("max_events_per_wait must be positive");
    return NULL;
  }
  if (num_lock_stripes == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("num_lock_stripes must be positive");
    return NULL;
  }
  if (num_reactor_threads == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("num_reactor_threads must be positive");
    return NULL;
  }
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  struct event_loop_s *loop = (struct event_loop_s *)_mem_alloc(
      mmgmt_procs, sizeof(struct event_loop_s));
  if (!loop) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate memory for event_loop");
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(loop, mmgmt_procs, err_str)) {
    _mem_free(mmgmt_procs, loop);
    return NULL;
  }

  loop->epfd = epoll_create1(EPOLL_CLOEXEC);
  if (loop->epfd < 0) {
    if (err_str) *err_str = CCOL_ERR_STR("epoll_create1 failed");
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return NULL;
  }

  loop->shutdown_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (loop->shutdown_efd < 0) {
    if (err_str) *err_str = CCOL_ERR_STR("eventfd failed");
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return NULL;
  }

  struct epoll_event ev;
  ev.data.ptr = NULL;
  ev.events = EPOLLIN;
  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->shutdown_efd, &ev) < 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("epoll_ctl failed registering shutdown eventfd");
    close(loop->shutdown_efd);
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return NULL;
  }

  mutex_init(loop->shutdown_lock);
  cond_var_init(loop->joined_cv);
  loop->shutdown_started = false;
  loop->joined = false;
  atomic_init(&loop->shutting_down, false);
  loop->max_events_per_wait = max_events_per_wait;
  atomic_init(&loop->pending_entry_frees, NULL);
  atomic_init(&loop->pending_reg_frees, NULL);
  atomic_init(&loop->global_gen, (size_t)0);
  atomic_init(&loop->fd_generation_counter, (uint64_t)0);
  atomic_init(&loop->next_queue_stripe, (size_t)0);
  atomic_init(&loop->reg_count, (size_t)0);
  loop->num_stripes = num_lock_stripes;
  loop->num_threads = num_reactor_threads;

  loop->stripes =
      _mem_calloc(mmgmt_procs, num_lock_stripes, sizeof(event_loop_stripe_t));
  if (!loop->stripes) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate lock stripe array");
    cond_var_destroy(loop->joined_cv);
    mutex_destroy(loop->shutdown_lock);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return NULL;
  }

  size_t stripes_created = 0;
  for (; stripes_created < num_lock_stripes; stripes_created++) {
    char *stripe_err = NULL;
    loop->stripes[stripes_created].fd_index =
        chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_int,
                          ccol_pointer, mmgmt_procs, NULL, &stripe_err);
    if (!loop->stripes[stripes_created].fd_index) {
      if (err_str)
        *err_str = stripe_err ? stripe_err
                              : CCOL_ERR_STR("Failed to create fd registry");
      _destroy_stripes(loop, mmgmt_procs, stripes_created);
      cond_var_destroy(loop->joined_cv);
      mutex_destroy(loop->shutdown_lock);
      close(loop->shutdown_efd);
      close(loop->epfd);
      _mem_free(mmgmt_procs, loop->m_procs);
      _mem_free(mmgmt_procs, loop);
      return NULL;
    }
    mutex_init(loop->stripes[stripes_created].lock);
    loop->stripes[stripes_created].queue_regs_head = NULL;
  }

  loop->thread_reached_gen = (_Atomic size_t *)_mem_calloc(
      mmgmt_procs, num_reactor_threads, sizeof(size_t));
  if (!loop->thread_reached_gen) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate thread epoch array");
    _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cond_var_destroy(loop->joined_cv);
    mutex_destroy(loop->shutdown_lock);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return NULL;
  }
  for (size_t i = 0; i < num_reactor_threads; i++) {
    atomic_init(&loop->thread_reached_gen[i], (size_t)0);
  }

  loop->threads =
      _mem_calloc(mmgmt_procs, num_reactor_threads, sizeof(thread_id_t));
  if (!loop->threads) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to allocate thread array");
    _mem_free(mmgmt_procs, loop->thread_reached_gen);
    _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cond_var_destroy(loop->joined_cv);
    mutex_destroy(loop->shutdown_lock);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return NULL;
  }

  size_t threads_created = 0;
  for (; threads_created < num_reactor_threads; threads_created++) {
    event_loop_thread_ctx *ctx =
        _mem_alloc(mmgmt_procs, sizeof(event_loop_thread_ctx));
    if (!ctx) break;
    ctx->loop = loop;
    ctx->thread_index = threads_created;
    if (thread_create(loop->threads[threads_created], _event_loop_thread_fn,
                      ctx) != 0) {
      _mem_free(mmgmt_procs, ctx);
      break;
    }
  }

  if (threads_created < num_reactor_threads) {
    if (err_str) *err_str = CCOL_ERR_STR("pthread_create failed");
    /* Roll back any threads that DID start: signal shutdown (the same
     * single-write mechanism event_loop_shutdown itself uses -- every
     * already-started thread will observe shutting_down at the top of its
     * own loop, whether or not it happens to personally see the eventfd
     * event, so a partial start is joined cleanly the same way a full
     * shutdown is) and join them before freeing anything they might still
     * be touching. */
    atomic_store(&loop->shutting_down, true);
    uint64_t one = 1;
    (void)write(loop->shutdown_efd, &one, sizeof(one));
    for (size_t i = 0; i < threads_created; i++) {
      thread_join(loop->threads[i]);
    }
    _mem_free(mmgmt_procs, loop->threads);
    _mem_free(mmgmt_procs, loop->thread_reached_gen);
    _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cond_var_destroy(loop->joined_cv);
    mutex_destroy(loop->shutdown_lock);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return NULL;
  }

  if (err_str) *err_str = NULL;
  return loop;
}

ccol_retval_t event_loop_shutdown(event_loop loop) {
  if (!loop) return ccol_invalid_args;

  mutex_lock(loop->shutdown_lock);
  bool is_leader = !loop->shutdown_started;
  loop->shutdown_started = true;
  mutex_unlock(loop->shutdown_lock);

  if (is_leader) {
    atomic_store(&loop->shutting_down, true);
    uint64_t one = 1;
    (void)write(loop->shutdown_efd, &one, sizeof(one));

    /* A single write is sufficient for every reactor thread, not just one --
     * but only because _event_loop_handle_event's NULL-entry branch
     * deliberately never drains shutdown_efd (see its own comment for the
     * real deadlock an earlier version of this code hit here: draining it
     * let whichever thread processed the event first reset the counter to
     * 0 before some other already-blocked thread's epoll_wait call had a
     * chance to observe it, leaving that thread stuck in epoll_wait
     * forever and this join loop hung on it). With the counter left
     * permanently non-zero once written, every thread's epoll_wait call --
     * already blocked or not yet called, in whatever order they run --
     * keeps seeing shutdown_efd as ready (level-triggered) until it
     * actually returns and this loop's shutting_down check (set above,
     * before this write) breaks it out. This was a real, reproduced hang
     * during this feature's own development (gdb thread-apply-all-bt on a
     * stuck test process pinned every remaining thread to this exact
     * epoll_wait call), not a theoretical concern -- see the multi-thread
     * shutdown tests in tests/cthreadcomm/tests.c for the regression
     * coverage this fix is verified against. */
    for (size_t i = 0; i < loop->num_threads; i++) {
      thread_join(loop->threads[i]);
    }

    mutex_lock(loop->shutdown_lock);
    loop->joined = true;
    cond_var_broadcast(loop->joined_cv);
    mutex_unlock(loop->shutdown_lock);
  } else {
    mutex_lock(loop->shutdown_lock);
    while (!loop->joined) {
      cond_var_wait(loop->joined_cv, loop->shutdown_lock);
    }
    mutex_unlock(loop->shutdown_lock);
  }

  return ccol_success;
}

void __event_loop_destroy(event_loop loop) {
  if (!loop) return;

  event_loop_shutdown(loop);

  /* Entries/regs deferred during the final round of batch processing (right
   * before shutting_down was observed) never got a chance to reach a
   * reclaim point on their own defer_gen's schedule. Every reactor thread
   * is joined now (no concurrent access possible from any of them any
   * more), so it's safe to free every remaining deferred item
   * unconditionally rather than leaking them -- see
   * _event_loop_free_all_pending's own comment for why no epoch check is
   * needed at this specific point. */
  _event_loop_free_all_pending(loop);

  /* Every reactor thread has been joined and every other in-flight
   * event_loop_shutdown caller has already returned too (the leader/
   * follower join protocol above guarantees this); no dispatch can be in
   * flight and no other thread can be touching this loop's registry.  Safe
   * to walk and free every remaining registration in every stripe without
   * any lock.
   *
   * chmap's separate-chaining storage is packed (see _fd_registry_find), so
   * the stored event_entry* is read via memcpy, not a direct pointer cast.
   * chashmap_begin_iter/it->_next_fn only free the iterator's own
   * bookkeeping as they walk; freeing what a stored value POINTS TO here
   * is this loop's responsibility, same as chmap_destroy below only frees
   * the map's copies of the int keys and pointer values, never the
   * event_entry structs those pointers reference. */
  for (size_t i = 0; i < loop->num_stripes; i++) {
    event_loop_stripe_t *stripe = &loop->stripes[i];

    char *iter_err = NULL;
    cmap_iterator *it = chashmap_begin_iter(stripe->fd_index, &iter_err);
    while (it) {
      event_entry *entry;
      memcpy(&entry, it->val_pair->ptr, sizeof(entry));
      if (entry->as.fd.read_reg) _event_reg_free(loop, entry->as.fd.read_reg);
      if (entry->as.fd.write_reg) _event_reg_free(loop, entry->as.fd.write_reg);
      mutex_destroy(entry->dispatch_lock);
      _mem_free(loop->m_procs, entry);
      it = it->_next_fn(it);
    }
    chmap_destroy(stripe->fd_index);

    event_reg *reg = stripe->queue_regs_head;
    while (reg) {
      event_reg *next = reg->loop_list_next;
      mutex_t *q_mtx;
      ccol_sel_waiter **q_head;
      _queue_sel_locate(&reg->sel, &q_mtx, &q_head);
      _sel_unlink_waiter(&reg->waiter_node, q_head, q_mtx);
      mutex_destroy(reg->owning_entry->dispatch_lock);
      _mem_free(loop->m_procs, reg->owning_entry);
      _event_reg_free(loop, reg);
      reg = next;
    }

    mutex_destroy(stripe->lock);
  }
  _mem_free(loop->m_procs, loop->stripes);
  _mem_free(loop->m_procs, loop->threads);
  _mem_free(loop->m_procs, loop->thread_reached_gen);

  cond_var_destroy(loop->joined_cv);
  mutex_destroy(loop->shutdown_lock);
  close(loop->shutdown_efd);
  close(loop->epfd);

  if (loop->m_procs) {
    ccol_free_t free_func = loop->m_procs->free;
    free_func(loop->m_procs);
    free_func(loop);
  } else {
    mem_free(loop);
  }
}
