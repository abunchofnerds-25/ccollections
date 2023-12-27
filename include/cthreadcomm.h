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

#pragma once

#include <common.h>
#include <time.h>

typedef struct circular_queue circular_queue;
typedef struct dynamic_queue dynamic_queue;
typedef struct channel channel;

typedef struct c_message_t {
  void* data;
  size_t size;
} c_message_t;

// Circular queue related functions
circular_queue* circular_queue_create_with_mprocs(
    size_t max_size, ccol_memmgmt_procs_t* mmgmt_procs, char** err_str);

#define circular_queue_create(max_size, err_str) \
  circular_queue_create_with_mprocs(max_size, NULL, err_str)

void __circular_queue_destroy(circular_queue* cq);

#define circular_queue_destroy(cq) \
  do {                             \
    __circular_queue_destroy(cq);  \
    cq = NULL;                     \
  } while (0)

// The following six functions do not perform any copy operations,
// hence the suffix 'zc' (zero copy). Please notice that these
// functions will be assigning NULL into '*msg'/'*target_buf'.
ccol_retval_t circq_send_zc(circular_queue* cq, c_message_t* msg);
ccol_retval_t circq_try_send_zc(circular_queue* cq, c_message_t* msg);
ccol_retval_t circq_timed_send_zc(circular_queue* cq, c_message_t* msg,
                                  struct timespec* timeout);

ccol_retval_t circq_recv_zc(circular_queue* cq, c_message_t* target_buf);
ccol_retval_t circq_try_recv_zc(circular_queue* cq, c_message_t* target_buf);
ccol_retval_t circq_timed_recv_zc(circular_queue* cq, c_message_t* target_buf,
                                  struct timespec* timeout);

ccol_retval_t circq_disable_sending(circular_queue* cq);
ccol_retval_t circq_enable_sending(circular_queue* cq);

size_t circq_msg_count(circular_queue* cq);

// Dynamic queue related functions
// Dynamic queues will try to accept messages as much as
// possible, unlike circular queues which start blocking the
// dispacher threads once they reach their capacities.
// A send call to a dynamic queue should never block,
// it should directly succeed or fail depending on the
// availability of memory.
dynamic_queue* dynamic_queue_create_with_mprocs(
    ccol_memmgmt_procs_t* mmgmt_procs, char** err_str);

#define dynamic_queue_create(err_str) \
  dynamic_queue_create_with_mprocs(NULL, err_str)

void __dynamic_queue_destroy(dynamic_queue* dq);

#define dynamic_queue_destroy(dq) \
  do {                            \
    __dynamic_queue_destroy(dq);  \
    dq = NULL;                    \
  } while (0)

ccol_retval_t dynmq_send_zc(dynamic_queue* dq, c_message_t* msg);

ccol_retval_t dynmq_recv_zc(dynamic_queue* dq, c_message_t* target_buf);
ccol_retval_t dynmq_try_recv_zc(dynamic_queue* dq, c_message_t* target_buf);
ccol_retval_t dynmq_timed_recv_zc(dynamic_queue* dq, c_message_t* target_buf,
                                  struct timespec* timeout);

ccol_retval_t dynmq_disable_sending(dynamic_queue* dq);
ccol_retval_t dynmq_enable_sending(dynamic_queue* dq);

int dynmq_msg_count(dynamic_queue* dq);

// Channel related functions
channel* channel_create_with_mprocs(size_t max_size,
                                    ccol_memmgmt_procs_t* mmgmt_procs,
                                    char** err_str);

#define channel_create(max_size, err_str) \
  channel_create_with_mprocs(max_size, NULL, err_str)

void __channel_destroy(channel* ch);

#define channel_destroy(ch) \
  do {                      \
    __channel_destroy(ch);  \
    ch = NULL;              \
  } while (0)

ccol_retval_t chan_send_zc(channel* ch, c_message_t* msg);
ccol_retval_t chan_try_send_zc(channel* ch, c_message_t* msg);
ccol_retval_t chan_timed_send_zc(channel* ch, c_message_t* msg,
                                 struct timespec* timeout);

ccol_retval_t chan_recv_zc(channel* ch, c_message_t* target_buf);
ccol_retval_t chan_try_recv_zc(channel* ch, c_message_t* target_buf);
ccol_retval_t chan_timed_recv_zc(channel* ch, c_message_t* target_buf,
                                 struct timespec* timeout);

typedef enum channel_direction {
  owner_to_workers = 0,
  workers_to_owner
} channel_direction;

ccol_retval_t chan_disable_sending(channel* ch, channel_direction d);
ccol_retval_t chan_enable_sending(channel* ch, channel_direction d);

int chan_msg_count(channel* ch, channel_direction d);
