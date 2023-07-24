/*
MIT License

Copyright (c) 2018 Danis Ozdemir

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
#include <pthread.h>
#include <stdlib.h>

#define mutex_t pthread_mutex_t
#define mutex_destroy(m) pthread_mutex_destroy(&m)
#define mutex_init(m) pthread_mutex_init(&m, NULL)
#define mutex_lock(m) pthread_mutex_lock(&m)
#define mutex_unlock(m) pthread_mutex_unlock(&m)

#define cond_var_t pthread_cond_t
#define cond_var_destroy(c) pthread_cond_destroy(&c)
#define cond_var_init(c) pthread_cond_init(&c, NULL)
#define cond_var_wait(c, m) pthread_cond_wait(&c, &m)
#define cond_var_timedwait(c, m, t) pthread_cond_timedwait(&c, &m, &t)
#define cond_var_signal(c) pthread_cond_signal(&c)

#define thread_id_t pthread_t
#define get_thread_id pthread_self

#define stringify(s) #s
#define x_stringify(s) stringify(s)
#define CERR_STR(x) (__FILE__ ":" x_stringify(__LINE__) " - " x)

#ifdef RUNNING_UNIT_TESTS
#include <assert.h>
#endif

const size_t max_allowed_cq_size = INT32_MAX;

typedef struct message {
  void* data;
  size_t size;
} message;

void add_duration_to_timespec(struct timespec* target,
                              struct timespec* duration) {
  static const long int max_nsecs = 1000000000;

  if (target->tv_nsec > max_nsecs) {
    target->tv_sec += target->tv_nsec / max_nsecs;
    target->tv_nsec = target->tv_nsec % max_nsecs;
  }

  if (duration->tv_nsec > max_nsecs) {
    duration->tv_sec += duration->tv_nsec / max_nsecs;
    duration->tv_nsec = duration->tv_nsec % max_nsecs;
  }

  target->tv_sec += duration->tv_sec;

  long int gap = max_nsecs - target->tv_nsec;

  if (gap > duration->tv_nsec) {
    target->tv_nsec += duration->tv_nsec;
  } else {
    ++target->tv_sec;
    target->tv_nsec = duration->tv_nsec - gap;
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

  ccol_memmgmt_procs_t* m_procs;

  message* msg_array;
  bool writing_disabled;
};

bool verify_circular_queue_create_inputs(size_t max_size,
                                         ccol_memmgmt_procs_t* mmgmt_procs,
                                         char** err_str) {
  if (max_size == 0) {
    if (err_str) {
      *err_str = CERR_STR("max_size should be positive");
    }
    return false;
  }

  if (max_size > max_allowed_cq_size) {
    if (err_str) {
      *err_str = CERR_STR("max_size can not exceed max_allowed_cq_size");
    }
    return false;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return false;
  }

  return true;
}

circular_queue* circular_queue_create_with_mprocs(
    size_t max_size, ccol_memmgmt_procs_t* mmgmt_procs, char** err_str) {
  if (!verify_circular_queue_create_inputs(max_size, mmgmt_procs, err_str)) {
    return NULL;
  }

  circular_queue* cq =
      (circular_queue*)_mem_alloc(mmgmt_procs, sizeof(circular_queue));
  if (!cq) {
    if (err_str) {
      *err_str = CERR_STR("Failed to allocate memory for circular_queue");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(cq, mmgmt_procs, err_str)) {
    _mem_free(mmgmt_procs, cq);
    return NULL;
  }

  cq->msg_array = (message*)_mem_alloc(mmgmt_procs, max_size * sizeof(message));
  if (!cq->msg_array) {
    if (err_str) {
      *err_str = CERR_STR("Failed to allocate memory for cq msg_array");
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

  if (err_str) {
    *err_str = NULL;
  }

  return cq;
}

void __circular_queue_destroy(circular_queue* cq) {
  if (cq) {
    if (cq->msg_array) {
      _mem_free(cq->m_procs, cq->msg_array);
      cq->msg_array = NULL;
    }

    mutex_destroy(cq->mutex);
    cond_var_destroy(cq->read_cond);
    cond_var_destroy(cq->write_cond);

    if (cq->m_procs) {
      ccol_memmgmt_procs_free_t free_func = cq->m_procs->free;
      free_func(cq->m_procs);
      free_func(cq);
    } else {
      mem_free(cq);
    }
  }
}

// This function should always be called while holding the mutex.
// Please notice that it's not exposed to the caller via the header file.
void _sendto_cq(circular_queue* cq, c_message_t* msg) {
  cq->msg_array[cq->write_index].data = msg->data;
  if (msg->data == NULL) {
    msg->size = 0;
  }
  cq->msg_array[cq->write_index++].size = msg->size;
  msg->data = NULL;  // The sender loses the ownership of the msg pointer.
  if (cq->write_index == cq->max_size) {
    cq->write_index = 0;
  }
  ++cq->msg_count;

  cond_var_signal(cq->read_cond);
}

bool verify_circq_send_zc_params(circular_queue* cq, c_message_t* msg) {
  if (!cq || !msg || (msg->size == 0 && msg->data != NULL)) {
    return false;
  }

  return true;
}

ccol_retval_t circq_send_zc(circular_queue* cq, c_message_t* msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  while (cq->msg_count == cq->max_size) {
    cond_var_wait(cq->write_cond, cq->mutex);
  }

  _sendto_cq(cq, msg);

  mutex_unlock(cq->mutex);

  return ccol_success;
}

ccol_retval_t circq_try_send_zc(circular_queue* cq, c_message_t* msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  // Assuming we won't have space for the new message.
  int result = ccol_container_full;

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

ccol_retval_t circq_timed_send_zc(circular_queue* cq, c_message_t* msg,
                                  struct timespec* timeout_duration) {
  if (!verify_circq_send_zc_params(cq, msg)) {
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

    while (cq->msg_count == cq->max_size) {
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

  _sendto_cq(cq, msg);

  mutex_unlock(cq->mutex);

  return ccol_success;
}

// This function should always be called while holding the mutex.
// Please notice that it's not exposed to the caller via the header file.
void _recvfrom_cq(circular_queue* cq, c_message_t* target_buf) {
  target_buf->data = cq->msg_array[cq->read_index].data;
  target_buf->size = cq->msg_array[cq->read_index++].size;
  if (cq->read_index == cq->max_size) {
    cq->read_index = 0;
  }

  --cq->msg_count;

  cond_var_signal(cq->write_cond);
}

bool verify_recvfrom_cq_zc_params(circular_queue* cq, c_message_t* target_buf) {
  if (!cq || !target_buf) {
    return false;
  }

  return true;
}

ccol_retval_t circq_recv_zc(circular_queue* cq, c_message_t* target_buf) {
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

ccol_retval_t circq_try_recv_zc(circular_queue* cq, c_message_t* target_buf) {
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

ccol_retval_t circq_timed_recv_zc(circular_queue* cq, c_message_t* target_buf,
                                  struct timespec* timeout) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
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
        mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
  }

  _recvfrom_cq(cq, target_buf);

  mutex_unlock(cq->mutex);

  return ccol_success;
}

ccol_retval_t circq_disable_sending(circular_queue* cq) {
  if (cq) {
    mutex_lock(cq->mutex);
    cq->writing_disabled = true;
    mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

ccol_retval_t circq_enable_sending(circular_queue* cq) {
  if (cq) {
    mutex_lock(cq->mutex);
    cq->writing_disabled = false;
    mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

size_t circq_msg_count(circular_queue* cq) {
  size_t result = -1;

  if (cq) {
    mutex_lock(cq->mutex);
    result = cq->msg_count;
    mutex_unlock(cq->mutex);
  }

  return result;
}

// Dynamic queue related section starts here.
typedef struct dllist_node {
  struct dllist_node* prev;
  message msg;
  struct dllist_node* next;
} dllist_node;

struct dynamic_queue {
  mutex_t mutex;
  cond_var_t read_cond;

  size_t msg_count;

  dllist_node* head;
  dllist_node* tail;

  ccol_memmgmt_procs_t* m_procs;

  bool writing_disabled;
};

ccol_retval_t append_msg_to_dq_tail(dynamic_queue* dq, c_message_t* msg) {
  dllist_node* new_elem =
      (dllist_node*)_mem_alloc(dq->m_procs, sizeof(dllist_node));
  if (!new_elem) {
    return ccol_not_enough_memory;
  }

  if (msg->data == NULL) {
    msg->size = 0;
  }

  new_elem->msg.data = msg->data;
  msg->data = NULL;
  new_elem->msg.size = msg->size;
  new_elem->next = NULL;

  if (!dq->head) {
#ifdef RUNNING_UNIT_TESTS
    assert(!dq->tail);
#endif
    new_elem->prev = NULL;
    dq->head = new_elem;
    dq->tail = new_elem;
  } else {
#ifdef RUNNING_UNIT_TESTS
    assert(dq->tail && !dq->tail->next);
#endif
    new_elem->prev = dq->tail;
    dq->tail->next = new_elem;
    dq->tail = new_elem;
  }

  return ccol_success;
}

ccol_retval_t remove_msg_from_dq_head(dynamic_queue* dq,
                                      c_message_t* target_buf) {
  if (!dq->head) {
#ifdef RUNNING_UNIT_TESTS
    assert(!dq->tail);
#endif
    return ccol_container_empty;
  }

#ifdef RUNNING_UNIT_TESTS
  assert(!dq->head->prev);
  assert(dq->tail && !dq->tail->next);
#endif

  dllist_node* node_to_be_freed = dq->head;

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

void destroy_dq_dllist(dynamic_queue* dq) {
  dllist_node* node_to_be_freed = NULL;
  while (dq->head) {
    node_to_be_freed = dq->head;
    dq->head = dq->head->next;
    _mem_free(dq->m_procs, node_to_be_freed);
  }
  dq->tail = NULL;
}

dynamic_queue* dynamic_queue_create_with_mprocs(
    ccol_memmgmt_procs_t* mmgmt_procs, char** err_str) {
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  dynamic_queue* dq =
      (dynamic_queue*)_mem_alloc(mmgmt_procs, sizeof(dynamic_queue));
  if (!dq) {
    if (err_str) {
      *err_str = CERR_STR("Failed to allocate memory for dynamic_queue");
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

  if (err_str) {
    *err_str = NULL;
  }

  return dq;
}

void __dynamic_queue_destroy(dynamic_queue* dq) {
  if (dq) {
    mutex_destroy(dq->mutex);
    cond_var_destroy(dq->read_cond);
    destroy_dq_dllist(dq);

    if (dq->m_procs) {
      ccol_memmgmt_procs_free_t free_func = dq->m_procs->free;
      free_func(dq->m_procs);
      free_func(dq);
    } else {
      mem_free(dq);
    }
  }
}

ccol_retval_t _sendto_dq(dynamic_queue* dq, c_message_t* msg) {
  ccol_retval_t retval = append_msg_to_dq_tail(dq, msg);

  if (retval == ccol_success) {
    ++dq->msg_count;
    cond_var_signal(dq->read_cond);
  }

  return retval;
}

bool verify_dynmq_send_zc_params(dynamic_queue* dq, c_message_t* msg) {
  if (!dq || !msg || (msg->size == 0 && msg->data != NULL)) {
    return false;
  }

  return true;
}

ccol_retval_t dynmq_send_zc(dynamic_queue* dq, c_message_t* msg) {
  if (!verify_dynmq_send_zc_params(dq, msg)) {
    return ccol_invalid_args;
  }

  mutex_lock(dq->mutex);

  if (dq->writing_disabled) {
    mutex_unlock(dq->mutex);
    return ccol_not_permitted;
  }

  _sendto_dq(dq, msg);

  mutex_unlock(dq->mutex);

  return ccol_success;
}

ccol_retval_t _recvfrom_dq(dynamic_queue* dq, c_message_t* target_buf) {
  ccol_retval_t retval = remove_msg_from_dq_head(dq, target_buf);

  if (retval == ccol_success) {
    --dq->msg_count;
  }

  return retval;
}

bool verify_recvfrom_dq_zc_params(dynamic_queue* dq, c_message_t* target_buf) {
  if (!dq || !target_buf) {
    return false;
  }

  return true;
}

ccol_retval_t dynmq_recv_zc(dynamic_queue* dq, c_message_t* target_buf) {
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

ccol_retval_t dynmq_try_recv_zc(dynamic_queue* dq, c_message_t* target_buf) {
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

ccol_retval_t dynmq_timed_recv_zc(dynamic_queue* dq, c_message_t* target_buf,
                                  struct timespec* timeout) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
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
        mutex_unlock(dq->mutex);
        return ccol_timed_out;
      }
    }
  }

  ccol_retval_t result = _recvfrom_dq(dq, target_buf);

  mutex_unlock(dq->mutex);

  return result;
}

ccol_retval_t dynmq_disable_sending(dynamic_queue* dq) {
  if (dq) {
    mutex_lock(dq->mutex);
    dq->writing_disabled = true;
    mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

ccol_retval_t dynmq_enable_sending(dynamic_queue* dq) {
  if (dq) {
    mutex_lock(dq->mutex);
    dq->writing_disabled = false;
    mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

int dynmq_msg_count(dynamic_queue* dq) {
  int result = -1;

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
  circular_queue* owner_to_workers_cq;
  circular_queue* workers_to_owner_cq;
  ccol_memmgmt_procs_t* m_procs;
};

channel* channel_create_with_mprocs(size_t max_size,
                                    ccol_memmgmt_procs_t* mmgmt_procs,
                                    char** err_str) {
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  channel* ch = (channel*)_mem_alloc(mmgmt_procs, sizeof(channel));
  if (!ch) {
    if (err_str) {
      *err_str = CERR_STR("Failed to allocate memory for channel");
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

void __channel_destroy(channel* ch) {
  if (ch) {
    circular_queue_destroy(ch->owner_to_workers_cq);
    circular_queue_destroy(ch->workers_to_owner_cq);

    if (ch->m_procs) {
      ccol_memmgmt_procs_free_t free_func = ch->m_procs->free;
      free_func(ch->m_procs);
      free_func(ch);
    } else {
      mem_free(ch);
    }
  }
}

ccol_retval_t chan_send_zc(channel* ch, c_message_t* msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_send_zc(ch->owner_to_workers_cq, msg);
  }

  return circq_send_zc(ch->workers_to_owner_cq, msg);
}

ccol_retval_t chan_try_send_zc(channel* ch, c_message_t* msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_try_send_zc(ch->owner_to_workers_cq, msg);
  }

  return circq_try_send_zc(ch->workers_to_owner_cq, msg);
}

ccol_retval_t chan_timed_send_zc(channel* ch, c_message_t* msg,
                                 struct timespec* timeout) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_timed_send_zc(ch->owner_to_workers_cq, msg, timeout);
  }

  return circq_timed_send_zc(ch->workers_to_owner_cq, msg, timeout);
}

ccol_retval_t chan_recv_zc(channel* ch, c_message_t* target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_recv_zc(ch->workers_to_owner_cq, target_buf);
  }

  return circq_recv_zc(ch->owner_to_workers_cq, target_buf);
}

ccol_retval_t chan_try_recv_zc(channel* ch, c_message_t* target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_try_recv_zc(ch->workers_to_owner_cq, target_buf);
  }

  return circq_try_recv_zc(ch->owner_to_workers_cq, target_buf);
}

ccol_retval_t chan_timed_recv_zc(channel* ch, c_message_t* target_buf,
                                 struct timespec* timeout) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (get_thread_id() == ch->owner_tid) {
    return circq_timed_recv_zc(ch->workers_to_owner_cq, target_buf, timeout);
  }

  return circq_timed_recv_zc(ch->owner_to_workers_cq, target_buf, timeout);
}

ccol_retval_t chan_disable_sending(channel* ch, channel_direction d) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (d == owner_to_workers) {
    circq_disable_sending(ch->owner_to_workers_cq);
  } else if (d == workers_to_owner) {
    circq_disable_sending(ch->workers_to_owner_cq);
  } else {
    return ccol_invalid_args;
  }

  return ccol_success;
}

ccol_retval_t chan_enable_sending(channel* ch, channel_direction d) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (d == owner_to_workers) {
    circq_enable_sending(ch->owner_to_workers_cq);
  } else if (d == workers_to_owner) {
    circq_enable_sending(ch->workers_to_owner_cq);
  } else {
    return ccol_invalid_args;
  }

  return ccol_success;
}

int chan_msg_count(channel* ch, channel_direction d) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (d == owner_to_workers) {
    return circq_msg_count(ch->owner_to_workers_cq);
  } else if (d == workers_to_owner) {
    return circq_msg_count(ch->workers_to_owner_cq);
  }

  return ccol_invalid_args;
}
