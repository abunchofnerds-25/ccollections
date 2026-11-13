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

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
//
// Thread-safe message passing between threads. ccircq is a fixed-size queue
// whose send blocks while it's full (that's its backpressure), cdynmq is a
// linked-list queue whose send never blocks, and cchan is two-way communication
// between an owner thread and a worker thread. Nothing is copied: A send hands
// over the ownership of a pointer. cselect() waits on several queues and fds,
// and ceventloop is a reactor that dispatches their readiness to callbacks.
#pragma GCC visibility push(default)

// A thread-safe circular queue of cmessage values with a fixed capacity. A
// send waits (or fails, for the try and timed forms) while it is full. Create
// it with ccircq_create() and destroy it with ccircq_destroy().
typedef struct ccircq ccircq;

// A thread-safe queue of cmessage values with no fixed capacity, kept in a
// linked list, so a send never waits for room. Create it with cdynmq_create()
// and destroy it with cdynmq_destroy().
typedef struct cdynmq cdynmq;

// A two-way channel between one owner thread and its worker threads, made of
// two circular queues. Each call picks its direction from the calling thread.
// Create it with cchan_create() and destroy it with cchan_destroy().
typedef struct cchan cchan;

// A message. A successful send sets the sender's data to NULL and the receiver
// owns (and frees) it. data NULL with size 0 is a valid sentinel message; data
// NULL with a size, or a pointer with size 0, is refused.
typedef struct cmessage {
  void *data;
  size_t size;
} cmessage;

// Creates a circular queue that holds at most max_size messages (at least 1,
// at most ccol_max_elem_count and SIZE_MAX / sizeof(cmessage), since the queue
// is one array). mmgmt_procs is your allocator table, or NULL for malloc and
// free; a table must set all four functions, and the queue keeps its own copy.
// Gives the queue, or NULL with an error string in *err_str when err_str isn't
// NULL. Destroy it with ccircq_destroy().
ccircq *ccircq_create_mp(size_t max_size, ccol_memmgmt_procs *mmgmt_procs,
                         char **err_str);

// Creates a circular queue that holds at most max_size messages (at least 1,
// at most ccol_max_elem_count and SIZE_MAX / sizeof(cmessage)), allocated with
// malloc and free. Evaluates to the queue, or to NULL with an error string in
// *err_str when err_str isn't NULL. Destroy it with ccircq_destroy().
#define ccircq_create(max_size, err_str) \
  ccircq_create_mp((max_size), NULL, (err_str))

// Internal: Destroys cq; ccircq_destroy() calls it and then clears the pointer.
// Use the macro instead.
void ccircq__destroy(ccircq *cq);

// Destroys the queue and sets cq to NULL, so a dangling pointer can't be used
// again. cq is evaluated once and must be a modifiable lvalue; NULL is fine.
// Messages left in the queue aren't freed, and it asserts when the queue isn't
// empty or a cselect() call or a ceventloop registration still watches it,
// because the watcher points at the queue's mutex. So drain it and call
// ceventloop_remove() first. The destroy waits for any callback of this queue
// that was already running when the remove returned, and counts the messages
// only after that; a write callback can still send in that time, so with a
// write registration wait for its on_removed before you drain and destroy. A
// callback of another registration may run this whole sequence. Destroying from
// a callback that dispatches this same queue is a fatal error, and holding a
// lock that one of its callbacks takes deadlocks.
#define ccircq_destroy(cq) \
  ccircq__destroy_impl(cq, ccol__uniq(ccol__cq_destroy_slot, __COUNTER__))

// Internal: The body of ccircq_destroy(), which calls ccircq__destroy(*slot)
// and clears the pointer. slot comes from ccol__uniq(), so destroy macros can
// nest without -Wshadow warnings, and cq is evaluated once.
#define ccircq__destroy_impl(cq, slot) \
  do {                                 \
    __typeof__(cq) *slot = &(cq);      \
    ccircq__destroy(*slot);            \
    *slot = NULL;                      \
  } while (0)

// Sends msg, the send for a producer that should wait while the queue is full
// (that's the backpressure). It waits with no time limit for room or for sends
// to be turned off. On success msg->data becomes NULL and the queue owns it; on
// failure you keep it. It gives ccol_success, ccol_invalid_args (NULL cq or
// msg, or a data/size pair that doesn't match), or ccol_not_permitted when
// sends are off.
ccol_retval ccircq_send(ccircq *cq, cmessage *msg);

// Sends msg without waiting, for a producer that must never block. On success
// msg->data becomes NULL and the queue owns it; on failure you keep it. It
// gives ccol_success, ccol_container_full when the queue is full,
// ccol_invalid_args (NULL cq or msg, or a data/size pair that doesn't match:
// NULL data needs size 0 and other data a size above 0), or ccol_not_permitted
// when sends are off.
ccol_retval ccircq_try_send(ccircq *cq, cmessage *msg);

// Sends msg, waiting at most timeout_us microseconds for room, for a producer
// that may wait but not for ever. The wait is on CLOCK_MONOTONIC, so a wall
// clock step doesn't change it. 0 doesn't wait and gives ccol_container_full
// for a full queue; UINT64_MAX (or any value too large for a deadline) waits
// with no end. On success msg->data becomes NULL and the queue owns it; on
// failure you keep it. It gives ccol_success, ccol_timed_out, ccol_invalid_args
// (NULL cq or msg, or a data/size pair that doesn't match), ccol_not_permitted
// when sends are off, or ccol_unexpected_failure with errno set when the wait
// itself fails.
ccol_retval ccircq_timed_send(ccircq *cq, cmessage *msg, uint64_t timeout_us);

// Receives a message into target_buf, for a consumer with nothing else to do:
// it waits with no time limit, and you then own target_buf->data and must free
// it. It gives ccol_success or ccol_invalid_args (NULL cq or target_buf).
// Turning sends off doesn't wake it.
ccol_retval ccircq_recv(ccircq *cq, cmessage *target_buf);

// Takes a message into target_buf if one is queued, without waiting, for a
// consumer that must not block (after cselect(), or in a loop callback). On
// success you own target_buf->data and must free it. It gives ccol_success,
// ccol_container_empty when the queue is empty, or ccol_invalid_args (NULL cq
// or target_buf).
ccol_retval ccircq_try_recv(ccircq *cq, cmessage *target_buf);

// Receives a message into target_buf, waiting at most timeout_us microseconds
// on CLOCK_MONOTONIC, for a consumer that also has other work. 0 doesn't wait
// and gives ccol_container_empty for an empty queue; UINT64_MAX (or any value
// too large for a deadline) waits with no end. On success you own
// target_buf->data and must free it. It gives ccol_success, ccol_timed_out,
// ccol_invalid_args (NULL cq or target_buf), or ccol_unexpected_failure with
// errno set when the wait itself fails.
ccol_retval ccircq_timed_recv(ccircq *cq, cmessage *target_buf,
                              uint64_t timeout_us);

// Turns sends off, so an owner can stop new work while consumers drain what is
// queued. Every blocked sender wakes and gives ccol_not_permitted; receives go
// on. A queue with sends off counts as ready for the write direction of
// cselect() and of a loop registration, the way POLLOUT is set on a pipe whose
// reader is gone, so the waiter's send fails at once. Gives ccol_success or
// ccol_invalid_args.
ccol_retval ccircq_disable_sending(ccircq *cq);

// Turns sends back on after ccircq_disable_sending() and wakes every blocked
// sender. A full queue is then not ready for writing until it has room. Gives
// ccol_success or ccol_invalid_args.
ccol_retval ccircq_enable_sending(ccircq *cq);

// Gives a snapshot of the number of queued messages, or (size_t)-1 for a NULL
// queue.
size_t ccircq_msg_count(ccircq *cq);

// Creates a dynamic queue (a doubly-linked list), for a producer that must
// never block: Its send fails only for lack of memory, at ccol_max_elem_count
// messages, or while sends are off. mmgmt_procs is your allocator table, used
// for the queue and every node a send adds, or NULL for malloc and free; a
// table must set all four functions, and the queue keeps its own copy. Gives
// the queue, or NULL with an error string in *err_str when err_str isn't NULL.
// Destroy it with cdynmq_destroy().
cdynmq *cdynmq_create_mp(ccol_memmgmt_procs *mmgmt_procs, char **err_str);

// Creates a dynamic queue (a doubly-linked list) allocated with malloc and
// free, for a producer that must never block. Evaluates to the queue, or to
// NULL with an error string in *err_str when err_str isn't NULL. Destroy it
// with cdynmq_destroy().
#define cdynmq_create(err_str) cdynmq_create_mp(NULL, (err_str))

// Internal: Destroys dq; cdynmq_destroy() calls it and then clears the pointer.
// Use the macro instead.
void cdynmq__destroy(cdynmq *dq);

// Destroys the queue and its nodes and sets dq to NULL, so a dangling pointer
// can't be used again. dq is evaluated once and must be a modifiable lvalue;
// NULL is fine. Messages left in the queue aren't freed, and it asserts when
// the queue isn't empty or a cselect() call or a ceventloop registration still
// watches it, so drain it and call ceventloop_remove() first. It waits for any
// callback of this queue that was already running when the remove returned,
// and counts the messages only after that; with a write registration, wait for
// its on_removed before you drain and destroy. Destroying from a callback that
// dispatches this same queue is a fatal error, and holding a lock that one of
// its callbacks takes deadlocks.
#define cdynmq_destroy(dq) \
  cdynmq__destroy_impl(dq, ccol__uniq(ccol__dq_destroy_slot, __COUNTER__))

// Internal: The body of cdynmq_destroy(), like ccircq__destroy_impl().
#define cdynmq__destroy_impl(dq, slot) \
  do {                                 \
    __typeof__(dq) *slot = &(dq);      \
    cdynmq__destroy(*slot);            \
    *slot = NULL;                      \
  } while (0)

// Appends msg without blocking, the only send an unbounded queue needs. On
// success msg->data becomes NULL and the queue owns it; on failure you keep it.
// It gives ccol_success, ccol_invalid_args (NULL dq or msg, or a data/size pair
// that doesn't match: NULL data needs size 0 and other data a size above 0),
// ccol_not_permitted when sends are off, ccol_container_full at
// ccol_max_elem_count messages, or ccol_not_enough_memory.
ccol_retval cdynmq_send(cdynmq *dq, cmessage *msg);

// Receives the message at the head into target_buf, for a consumer with
// nothing else to do: It waits with no time limit, and you then own
// target_buf->data and must free it. It gives ccol_success or
// ccol_invalid_args (NULL dq or target_buf). Turning sends off doesn't wake it.
ccol_retval cdynmq_recv(cdynmq *dq, cmessage *target_buf);

// Takes the message at the head into target_buf if there is one, without
// waiting, for a consumer that must not block (after cselect(), or in a loop
// callback). On success you own target_buf->data and must free it. It gives
// ccol_success, ccol_container_empty when the queue is empty (target_buf is
// left alone), or ccol_invalid_args (NULL dq or target_buf).
ccol_retval cdynmq_try_recv(cdynmq *dq, cmessage *target_buf);

// Receives the message at the head into target_buf, waiting at most
// timeout_us microseconds on CLOCK_MONOTONIC, for a consumer that also has
// other work. 0 doesn't wait and gives ccol_container_empty for an empty
// queue; UINT64_MAX (or any value too large for a deadline) waits with no end.
// On success you own target_buf->data and must free it. It gives
// ccol_success, ccol_timed_out, ccol_invalid_args (NULL dq or target_buf), or
// ccol_unexpected_failure with errno set when the wait itself fails.
ccol_retval cdynmq_timed_recv(cdynmq *dq, cmessage *target_buf,
                              uint64_t timeout_us);

// Turns sends off, so an owner can stop new work while consumers drain what is
// queued; every later send gives ccol_not_permitted and receives go on. A
// queue with sends off counts as ready for the write direction of cselect()
// and of a loop registration, the way POLLOUT is set on a pipe whose reader is
// gone, so the waiter's send fails at once. Gives ccol_success or
// ccol_invalid_args.
ccol_retval cdynmq_disable_sending(cdynmq *dq);

// Turns sends back on after cdynmq_disable_sending(). Gives ccol_success or
// ccol_invalid_args.
ccol_retval cdynmq_enable_sending(cdynmq *dq);

// Gives a snapshot of the number of queued messages, or (size_t)-1 for a NULL
// queue.
size_t cdynmq_msg_count(cdynmq *dq);

// Creates a cchan, for two-way traffic between an owner thread and its
// workers: Two circular queues of at most max_size messages each,
// cchan_owner_to_workers and cchan_workers_to_owner. The calling thread becomes
// the owner and every other thread is a worker; each call picks its direction
// from the caller's thread ID, so no call names one. The owner must stay alive
// while the channel is in use, since the OS can give its ID to a new thread,
// which would then be routed as the owner. max_size is at least 1 and at most
// ccol_max_elem_count and SIZE_MAX / sizeof(cmessage). mmgmt_procs is your
// allocator table, or NULL for malloc and free; a table must set all four
// functions, and the channel keeps its own copy. Gives the channel, or NULL
// with an error string in *err_str when err_str isn't NULL. Destroy it with
// cchan_destroy().
cchan *cchan_create_mp(size_t max_size, ccol_memmgmt_procs *mmgmt_procs,
                       char **err_str);

// Creates a cchan allocated with malloc and free: Two circular queues of at
// most max_size messages each (at least 1). The calling thread becomes the
// owner, every other thread is a worker, and each later call picks its
// direction from the caller's thread, so the owner must stay alive while the
// channel is in use. Evaluates to the channel, or to NULL with an error string
// in *err_str when err_str isn't NULL. Destroy it with cchan_destroy().
#define cchan_create(max_size, err_str) \
  cchan_create_mp((max_size), NULL, (err_str))

// Internal: Destroys ch; cchan_destroy() calls it and then clears the pointer.
// Use the macro instead.
void cchan__destroy(cchan *ch);

// Destroys the channel and both its queues and sets ch to NULL, so a dangling
// pointer can't be used again. ch is evaluated once and must be a modifiable
// lvalue; NULL is fine. Messages left in either queue aren't freed, and it
// asserts when a queue isn't empty or a cselect() call or a ceventloop
// registration still watches it, so drain both directions and remove every
// registration first. It waits for any callback of the channel that was
// already running when the remove returned. Destroying from a callback that
// dispatches one of its directions is a fatal error.
#define cchan_destroy(ch) \
  cchan__destroy_impl(ch, ccol__uniq(cchan__destroy_slot, __COUNTER__))

// Internal: The body of cchan_destroy(), like ccircq__destroy_impl().
#define cchan__destroy_impl(ch, slot) \
  do {                                \
    __typeof__(ch) *slot = &(ch);     \
    cchan__destroy(*slot);            \
    *slot = NULL;                     \
  } while (0)

// Sends msg on the calling thread's outgoing queue of ch: The owner thread
// sends to cchan_owner_to_workers and any other thread to
// cchan_workers_to_owner. It waits with no time limit while that queue is
// full, and wakes when sends on it are turned off. On success msg->data becomes
// NULL and the channel owns it; on failure you keep it. It gives ccol_success,
// ccol_invalid_args (NULL ch or msg, or a data/size pair that doesn't match:
// NULL data needs size 0 and other data a size above 0), or ccol_not_permitted
// when sends on that direction are off.
ccol_retval cchan_send(cchan *ch, cmessage *msg);

// Sends msg on the calling thread's outgoing queue of ch without waiting: The
// owner thread sends to cchan_owner_to_workers and any other thread to
// cchan_workers_to_owner. On success msg->data becomes NULL and the channel
// owns it; on failure you keep it. It gives ccol_success, ccol_container_full
// when that queue is full, ccol_invalid_args (NULL ch or msg, or a data/size
// pair that doesn't match), or ccol_not_permitted when sends on that direction
// are off.
ccol_retval cchan_try_send(cchan *ch, cmessage *msg);

// Sends msg on the calling thread's outgoing queue of ch (the owner thread to
// cchan_owner_to_workers, any other thread to cchan_workers_to_owner), waiting
// at most timeout_us microseconds on CLOCK_MONOTONIC for room. 0 doesn't wait
// and gives ccol_container_full for a full queue; UINT64_MAX (or any value too
// large for a deadline) waits with no end. On success msg->data becomes NULL
// and the channel owns it; on failure you keep it. It gives ccol_success,
// ccol_timed_out, ccol_invalid_args, ccol_not_permitted when sends on that
// direction are off, or ccol_unexpected_failure with errno set when the wait
// itself fails.
ccol_retval cchan_timed_send(cchan *ch, cmessage *msg, uint64_t timeout_us);

// Receives into target_buf from the calling thread's incoming queue of ch: The
// owner thread receives from cchan_workers_to_owner and any other thread from
// cchan_owner_to_workers. It waits with no time limit for a message, and you
// then own target_buf->data and must free it. It gives ccol_success or
// ccol_invalid_args (NULL ch or target_buf).
ccol_retval cchan_recv(cchan *ch, cmessage *target_buf);

// Takes a message into target_buf from the calling thread's incoming queue of
// ch (the owner thread from cchan_workers_to_owner, any other thread from
// cchan_owner_to_workers) without waiting. On success you own
// target_buf->data and must free it. It gives ccol_success,
// ccol_container_empty when that queue is empty, or ccol_invalid_args (NULL ch
// or target_buf).
ccol_retval cchan_try_recv(cchan *ch, cmessage *target_buf);

// Receives into target_buf from the calling thread's incoming queue of ch (the
// owner thread from cchan_workers_to_owner, any other thread from
// cchan_owner_to_workers), waiting at most timeout_us microseconds on
// CLOCK_MONOTONIC. 0 doesn't wait and gives ccol_container_empty for an empty
// queue; UINT64_MAX (or any value too large for a deadline) waits with no end.
// On success you own target_buf->data and must free it. It gives
// ccol_success, ccol_timed_out, ccol_invalid_args (NULL ch or target_buf), or
// ccol_unexpected_failure with errno set when the wait itself fails.
ccol_retval cchan_timed_recv(cchan *ch, cmessage *target_buf,
                             uint64_t timeout_us);

// The two directions of a cchan, named for the control calls that take one.
typedef enum cchan_direction {
  cchan_owner_to_workers = 0,
  cchan_workers_to_owner
} cchan_direction;

// Turns sends on direction d of ch off, so one side can stop new work while
// the other drains what is queued; the other direction isn't touched. Every
// sender blocked on d wakes and gives ccol_not_permitted, and so does every
// later send on d; receives go on. A direction with sends off counts as ready
// for writing in cselect() and in a loop registration, so the waiter's send
// fails at once. Gives ccol_success, or ccol_invalid_args for a NULL ch or a
// bad d.
ccol_retval cchan_disable_sending(cchan *ch, cchan_direction d);

// Turns sends on direction d of ch back on after cchan_disable_sending() and
// wakes every sender blocked on d; the other direction isn't touched. Gives
// ccol_success, or ccol_invalid_args for a NULL ch or a bad d.
ccol_retval cchan_enable_sending(cchan *ch, cchan_direction d);

// Gives a snapshot of the message count of direction d, or (size_t)-1 for a
// NULL ch or a bad d.
size_t cchan_msg_count(cchan *ch, cchan_direction d);

// The kind of object a cselectable holds.
typedef enum {
  cselectable_type_ccircq,
  cselectable_type_cdynmq,
  cselectable_type_fd,
} cselectable_type;

// The direction a selectable waits on. cselect_read waits for a message to
// receive. cselect_write waits until a send doesn't have to wait: The queue has
// room (a ccircq below max_size, a cdynmq below ccol_max_elem_count) or its
// sends are off, in which case the send gives ccol_not_permitted at once.
// cselect() neither consumes nor reserves anything, so after it wakes you make
// the non-blocking call yourself (ccircq_try_recv(), cdynmq_try_recv(),
// ccircq_try_send() or cdynmq_send()) and check its result; another thread can
// win the race in between, as with select(2).
typedef enum {
  cselect_read,
  cselect_write,
} cselect_dir;

// One queue or fd that cselect() and ceventloop watch. Build it with
// ccircq_selectable(), cdynmq_selectable(), cselectable_fd() or
// cchan_selectable(). fd must be at least 0.
typedef struct {
  cselectable_type type;
  cselect_dir dir;
  union {
    ccircq *cq;
    cdynmq *dq;
    int fd;
  };
} cselectable;

// Evaluates to a cselectable that wraps the circular queue q_ for a wait in
// direction sel_dir (cselect_read: A message to receive; cselect_write: Room to
// send, or sends turned off), to pass to cselect() or ceventloop_add(). After
// the wait reports it ready, call ccircq_try_recv() or ccircq_try_send()
// yourself and check the result.
#define ccircq_selectable(q_, sel_dir) \
  ((cselectable){.type = cselectable_type_ccircq, .dir = (sel_dir), .cq = (q_)})

// Evaluates to a cselectable that wraps the dynamic queue q_ for a wait in
// direction sel_dir (cselect_read: A message to receive; cselect_write: Room to
// send, or sends turned off), to pass to cselect() or ceventloop_add(). After
// the wait reports it ready, call cdynmq_try_recv() or cdynmq_send() yourself
// and check the result.
#define cdynmq_selectable(q_, sel_dir) \
  ((cselectable){.type = cselectable_type_cdynmq, .dir = (sel_dir), .dq = (q_)})

// Evaluates to a cselectable that wraps the file descriptor fd_ (at least 0)
// for a wait in direction sel_dir, so cselect() and the loop can watch fds
// beside queues. cselect_read watches EPOLLIN and cselect_write EPOLLOUT; both
// also get EPOLLERR and EPOLLHUP, and a read selectable EPOLLRDHUP. The library
// only reports readiness; you do the read or write on the fd.
#define cselectable_fd(fd_, sel_dir) \
  ((cselectable){.type = cselectable_type_fd, .dir = (sel_dir), .fd = (fd_)})

// Gives a selectable for the queue of ch that the calling thread reads from
// (cselect_read, like cchan_recv()) or writes to (cselect_write, like
// cchan_send()). The queue is resolved once, here, and the selectable keeps
// watching it whichever thread waits on it later. In an event loop callback,
// receive or send with ccircq_try_recv() or ccircq_try_send() on sel.cq, never
// a cchan_ call: The loop's threads count as workers, so a cchan_ call there
// can pick the other queue. A NULL ch gives a selectable that cselect()
// refuses.
cselectable cchan_selectable(cchan *ch, cselect_dir dir);

// Waits until one of the n selectables is ready for its own direction and sets
// *ready_index; the caller then makes its own non-blocking receive, send, read
// or write and checks its result. An array may mix directions, queues and fds.
// Ready selectables share the calls fairly: Each call scans in rotation order
// from a start that moves per call, so a selectable that stays ready wins its
// share. Which one a call reports isn't specified. Each producer action wakes
// exactly one waiter, and a waiter that leaves without acting hands the wake
// on. n goes from 1 to INT_MAX. It gives ccol_success, ccol_invalid_args (a
// NULL or zero argument, a NULL queue, a negative fd, an unknown type or dir),
// ccol_not_enough_memory (nothing linked stays linked, no message moves), or
// ccol_unexpected_failure when a system call or a primitive init fails. Without
// fds it uses only condition variables. With fds it first asks poll(2) without
// waiting and uses epoll (kqueue on the BSDs and macOS) only when it must wait,
// so every fd must be something epoll can watch: A regular file or a directory
// gives ccol_unexpected_failure. The wait has no time limit. See also
// cselect_timed().
ccol_retval cselect(size_t *ready_index, size_t n, cselectable *selectables);

// Waits at most timeout_us microseconds on CLOCK_MONOTONIC until one of the n
// selectables (1 to INT_MAX; queues and fds in any mix of directions) is ready
// for its own direction, and sets *ready_index. It neither receives nor sends:
// you then make your own non-blocking receive, send, read or write and check
// its result, since another thread can win the race. Ready selectables share
// the calls fairly, and which one a call reports isn't specified. UINT64_MAX
// waits with no limit, and 0 polls once, still reporting what is ready at the
// moment of the call. Any other value waits at least that long, even past the
// INT_MAX milliseconds of one kernel wait, and looks once more at every
// selectable before it reports a timeout. It gives ccol_success,
// ccol_timed_out (*ready_index is left alone), ccol_invalid_args (a NULL or
// zero argument, a NULL queue, a negative fd, an unknown type or dir),
// ccol_not_enough_memory (nothing linked stays linked, no message moves), or
// ccol_unexpected_failure when a system call, a primitive init or the timed
// wait fails; an fd that epoll can't watch, such as a regular file or a
// directory, gives ccol_unexpected_failure.
ccol_retval cselect_timed(size_t *ready_index, size_t n,
                          cselectable *selectables, uint64_t timeout_us);

// Waits with no time limit until one of the selectables listed after
// ready_index is ready, for a fixed set written at the call site, for example
// cselect_va(&idx, ccircq_selectable(q0, cselect_read),
// cchan_selectable(ch, cselect_read)). It sets *ready_index to the position of
// the ready one in the list and doesn't receive or send; you make that call
// yourself and check its result. It evaluates to a ccol_retval: ccol_success,
// ccol_invalid_args (a NULL ready_index, a NULL queue, a negative fd, an
// unknown type or dir), ccol_not_enough_memory, or ccol_unexpected_failure
// when a system call fails or an fd can't be watched by epoll (a regular file
// or a directory). Each argument is evaluated once. It uses a statement
// expression, so it needs GNU C.
#define cselect_va(ready_index, ...)                                       \
  __extension__({                                                          \
    cselectable cselect__arr[] = {__VA_ARGS__};                            \
    cselect((ready_index), sizeof(cselect__arr) / sizeof(cselect__arr[0]), \
            cselect__arr);                                                 \
  })

// Waits at most timeout_us microseconds until one of the selectables listed
// after timeout_us is ready, for a fixed set written at the call site, and
// sets *ready_index to its position in the list. UINT64_MAX waits with no
// limit and 0 polls once. It doesn't receive or send; you make that call
// yourself and check its result. It evaluates to a ccol_retval: ccol_success,
// ccol_timed_out when none is ready in time, ccol_invalid_args (a NULL
// ready_index, a NULL queue, a negative fd, an unknown type or dir),
// ccol_not_enough_memory, or ccol_unexpected_failure when a system call fails
// or an fd can't be watched by epoll. Each argument is evaluated once. It uses
// a statement expression, so it needs GNU C.
#define cselect_timed_va(ready_index, timeout_us, ...)            \
  __extension__({                                                 \
    cselectable cselect__arr[] = {__VA_ARGS__};                   \
    cselect_timed((ready_index),                                  \
                  sizeof(cselect__arr) / sizeof(cselect__arr[0]), \
                  cselect__arr, (timeout_us));                    \
  })

// A long-lived reactor. Exactly one poller thread waits on epoll (kqueue on the
// BSDs and macOS), on purpose: Many threads waiting on one epoll set all wake
// for one event, and EPOLLEXCLUSIVE doesn't help with that. With
// num_reactor_threads > 1 the callbacks run on a separate pool of dispatch
// workers, and two things still hold. The callbacks of one registration never
// run at the same time, and neither do the read and the write registration of
// one fd, so state shared across both directions of an fd needs no lock. And a
// removed registration's fd number can be reused at once: A dispatch checks
// liveness right before it calls a callback, so a stale dispatch does nothing,
// and the loop never collects a second dispatch for an entry that has one in
// flight, which makes ceventloop_modify from a callback safe. In every
// configuration the calls that take a ceventloop_reg resolve it through a
// generation-tagged slot table first, so threads may race them on one handle,
// or use it long after a remove, and the loser just gets ccol_invalid_args (or
// generation 0). Like cselect(), the loop only reports readiness; your callback
// does the receive or the send.
typedef struct ceventloop__s ceventloop__s;

// The loop handle: A slot index and a generation packed into a value. It isn't
// a pointer, so never cast it. Compare it with CEVENTLOOP_NULL (0). Every use
// is resolved through the library's slot table, so a stale handle is detected
// and never dereferenced.
typedef uint64_t ceventloop;

// "No loop", the failure value of a create.
#define CEVENTLOOP_NULL ((ceventloop)0)

// The handle of one registration, the same design as ceventloop scoped to one
// loop. Its slot is freed by ceventloop_remove, after which every call with it
// gives ccol_invalid_args (generation 0).
typedef uint64_t ceventloop_reg;

// "No registration", the failure value of ceventloop_add.
#define CEVENTLOOP_REG_NULL ((ceventloop_reg)0)

// Runs when a registration becomes readable. The callback does its own read()
// or recv() on sel->fd, or its own ccircq_try_recv() or cdynmq_try_recv() on
// sel->cq or sel->dq (for a channel queue that's ccircq_try_recv(), never
// cchan_try_recv()), and copes with finding nothing, since another consumer may
// have won. Several sends can collapse into one notification; as long as a
// callback moved a message and the queue is still ready, the loop dispatches
// again, so receiving one message per call still drains a burst. reg is the
// registration's own handle, valid even in a dispatch that runs before
// ceventloop_add has returned. sel->dir is the current direction (modify can
// change it), and arg is what you passed to ceventloop_add. The same parameters
// apply to the writable and error callbacks.
typedef void (*ceventloop_readable_fn)(ceventloop loop, ceventloop_reg reg,
                                       cselectable *sel, void *arg);

// Runs when a registration becomes writable. The callback does its own write()
// or send(), or for a queue its own ccircq_try_send() or cdynmq_send() (the
// queue may have room, or its sends may be off and the send gives
// ccol_not_permitted), and copes with a full queue, since another producer may
// have won. reg is the registration's own handle, sel->dir its current
// direction, and arg what you passed to ceventloop_add.
typedef void (*ceventloop_writable_fn)(ceventloop loop, ceventloop_reg reg,
                                       cselectable *sel, void *arg);

// Runs on a condition that the registration's direction has no other handler
// for. EPOLLERR and EPOLLHUP come here when the direction isn't also ready, and
// so does EPOLLRDHUP (a peer's close) for a read registration with no
// on_readable. When the direction is ready too, its own handler wins, so bytes
// that arrived before the error are still delivered. With on_error NULL, an
// error or hang-up goes to the direction's handler, as libevent and libuv do,
// and that handler's read or write reports it; a read or write also clears a
// socket error, so a transient one costs one failed receive. With handlers for
// both directions one event reaches on_readable and then on_writable. Queue
// selectables never get here. A registration may carry only on_error, to hear
// that an fd died. Conditions are level-triggered: The loop keeps reporting one
// until it clears or you remove or pause the registration, so a handler that
// reads an end of file must remove the registration (before closing the fd). On
// the BSDs and macOS an error-only registration may hear each condition only
// once. reg is the registration's own handle, sel its selectable, and arg what
// you passed to ceventloop_add.
typedef void (*ceventloop_error_fn)(ceventloop loop, ceventloop_reg reg,
                                    cselectable *sel, void *arg);

// Runs exactly once for every registration that was really added, when the
// loop can prove that you may free arg: After a ceventloop_remove (from any
// thread, a callback of the same registration included) once every dispatch in
// flight or already collected has finished, or during a destroy of the loop. It
// never runs inside ceventloop_remove, but on the reactor thread a bounded time
// later; ceventloop_destroy runs every due one before it returns. The thread
// holds no lock of this module, so on_removed may take any lock of yours.
// Destroying the owning loop from it is fatal. With on_removed NULL you get
// only the weaker promise of ceventloop_remove().
typedef void (*ceventloop_removed_fn)(void *arg);

// The callbacks of one registration, each of which may be NULL. For an fd the
// loop doesn't arm a direction without a handler of its own. An fd event that
// no handler can take is dropped and the loop stops watching that registration
// (a hang-up would otherwise repeat on every wait) until ceventloop_resume() or
// a successful ceventloop_modify(). on_removed may be NULL when arg needs no
// notice before it's freed.
typedef struct ceventloop_handlers {
  ceventloop_readable_fn on_readable;
  ceventloop_writable_fn on_writable;
  ceventloop_error_fn on_error;
  ceventloop_removed_fn on_removed;
} ceventloop_handlers;

// Creates a loop and starts its threads, so it's ready to dispatch when the
// call returns. num_reactor_threads is the total thread count: 1 means one
// thread polls and runs every callback (no dispatch pool at all), and more
// means one poller plus num_reactor_threads - 1 dispatch workers. Never call
// ceventloop_shutdown from a callback. The registry of fds and entries has
// num_lock_stripes (at least 1) independent mutex and map pairs, and each fd or
// queue bridge belongs to one stripe, so add, remove and modify for different
// fds run in parallel. max_events_per_wait (1 to INT_MAX) sizes the poller's
// batch buffer. mmgmt_procs is your allocator table, or NULL for malloc and
// free; a table must set all four functions. It gives the loop handle, or
// CEVENTLOOP_NULL with an error string in *err_str (when err_str isn't NULL)
// on bad arguments, a failed allocation or system call, when the process has
// no thread-specific key left for the two keys every loop needs (created once
// per process), and after the library's exit destructors have released the
// slot table. Destroy the loop with ceventloop_destroy().
ceventloop ceventloop_create_mp(size_t max_events_per_wait,
                                size_t num_lock_stripes,
                                size_t num_reactor_threads,
                                ccol_memmgmt_procs *mmgmt_procs,
                                char **err_str);

// Creates an event loop allocated with malloc and free and starts its
// threads, so it's ready to dispatch when the call returns.
// num_reactor_threads is the total thread count: 1 runs every callback on the
// one poller thread, and more adds num_reactor_threads - 1 dispatch workers.
// num_lock_stripes (at least 1) is the number of independent registry locks,
// so registrations of different fds can change in parallel, and
// max_events_per_wait (1 to INT_MAX) sizes the poller's batch. Evaluates to
// the loop handle, or to CEVENTLOOP_NULL with an error string in *err_str when
// err_str isn't NULL. Destroy the loop with ceventloop_destroy().
#define ceventloop_create(max_events_per_wait, num_lock_stripes,  \
                          num_reactor_threads, err_str)           \
  ceventloop_create_mp((max_events_per_wait), (num_lock_stripes), \
                       (num_reactor_threads), NULL, (err_str))

// Registers sel with the loop for one direction, sel.dir, so its readiness
// reaches your callbacks, and gives the handle. For both directions of one fd,
// add twice; to change direction over time, use ceventloop_modify. A second
// registration for a direction already in use on the same fd is refused. Queues
// have no such limit: Any number of registrations, in any loops, and cselect()
// calls can watch one queue. Each producer or consumer call wakes exactly one
// listener; after its callback, a listener hands the wake on while the queue is
// still ready, and so does one that is removed or whose loop shuts down before
// it acted. A registration whose callback moved a message is dispatched again
// while the queue stays ready, and a wake that every listener declines stops
// after visiting each once. A write registration that always sends is
// dispatched while the queue has room, like a writable fd. Sends turned off
// wake a write registration once. The registration is live before the call
// returns, so a callback may run first; it gets its handle as reg. On failure
// it gives CEVENTLOOP_REG_NULL and *err_str names the class: "shut down",
// "direction already in use", "failed to allocate", or "a system call failed"
// (a regular file or a directory always fails that way). Thread-safe, also from
// a callback.
ceventloop_reg ceventloop_add(ceventloop loop, cselectable sel,
                              ceventloop_handlers handlers, void *arg,
                              char **err_str);

// An identity token for the fd behind reg, for your own bookkeeping across fd
// reuse. It's unique in the loop, made when the fd is first registered, shared
// by every registration on that fd and kept across ceventloop_modify. Stamp a
// connection with it after the add and compare later. Correctness never depends
// on it. It gives 0 (never a real generation) for a NULL, removed or stale reg
// or loop, whatever thread removed it.
uint64_t ceventloop_reg_generation(ceventloop loop, ceventloop_reg reg);

// Changes the direction of an fd registration (a queue's direction is part of
// its identity, so a queue gives ccol_invalid_args; remove and add instead).
// Later callbacks see the new sel->dir. The fd's interest mask is recomputed
// with no moment where the fd isn't registered. It also re-arms a registration
// that the loop stopped watching after an fd event that none of its handlers
// could take, even when new_dir is the current direction. It gives
// ccol_success, ccol_invalid_args for a bad or stale loop or reg, a queue
// registration or a bad new_dir, and ccol_not_permitted when another
// registration on the fd holds new_dir. Thread-safe, also from a callback.
ccol_retval ceventloop_modify(ceventloop loop, ceventloop_reg reg,
                              cselect_dir new_dir);

// Stops event delivery for an fd registration and keeps it whole: Its slot,
// its count in ceventloop_reg_count and its generation stay, and only the fd's
// interest mask leaves it out. No callback runs for it while paused; the other
// direction of the fd isn't touched. It's the cheap form of a remove followed
// by an add (no allocation, no registry churn, one epoll_ctl on one thread).
// Pausing a paused registration succeeds. It gives ccol_success, or
// ccol_invalid_args for a bad or stale loop or reg (one removed at the same
// time included) or a queue registration. Thread-safe, also from a callback.
// Undo it with ceventloop_resume().
ccol_retval ceventloop_pause(ceventloop loop, ceventloop_reg reg);

// Puts a registration that ceventloop_pause() paused back into the fd's
// interest mask, so its callbacks run again, and re-arms one that the loop
// stopped watching after an fd event that none of its handlers could take.
// Resuming anything else succeeds and does nothing. It gives ccol_success, or
// ccol_invalid_args for a bad or stale loop or reg (the connection may have
// closed meanwhile) or a queue registration. Thread-safe, also from a callback
// and from a thread other than the one that paused it.
ccol_retval ceventloop_resume(ceventloop loop, ceventloop_reg reg);

// Removes a registration. It may run from the registration's own callback,
// from any thread, and during a dispatch of it. After it returns the loop calls
// none of its callbacks again, collected ones included, but it doesn't wait for
// a dispatch already running: To free an arg that a callback uses, wait for
// on_removed (waiting here could deadlock on your locks). From the
// registration's own callback, or when it was never dispatched, you may free
// arg at once. It never closes the fd. Remove every registration of an fd
// before you close it; once the last one is gone the loop has stopped watching
// the fd, so the number can be registered again independently. A registration
// whose fd was closed first keeps the number, and an add that reuses it fails.
// For a queue the waiter is unlinked before the call returns, and destroying
// the queue waits for a callback still running, so drain, remove, destroy is
// safe from any thread that isn't running one of its callbacks. It gives
// ccol_success (also for a call that races another remove of the same reg) or
// ccol_invalid_args for a bad or stale loop or reg, including one an earlier
// call already removed.
ccol_retval ceventloop_remove(ceventloop loop, ceventloop_reg reg);

// Gives the number of live registrations (one fd with both directions counts
// 2), or (size_t)-1 for a bad or stale loop.
size_t ceventloop_reg_count(ceventloop loop);

// Stops the poller and the dispatch workers and joins them, after each finishes
// its current job, so no dispatch is in flight when it returns. It may be
// called more than once; ceventloop_destroy calls it when needed. From then on
// the loop's queue registrations take no wake (one they held goes to the
// queue's next listener), registrations stay until removed or destroyed, and
// ceventloop_add is refused. It gives ccol_success, ccol_invalid_args for a bad
// or stale loop, or ccol_not_permitted from a callback on one of the loop's
// threads, which would join itself.
ccol_retval ceventloop_shutdown(ceventloop loop);

// Internal: Destroys the loop; ceventloop_destroy() calls it and then clears
// the handle. It shuts the loop down (the same drain as ceventloop_shutdown,
// with loop still resolving, so a callback can remove its registrations, close
// its fd and free arg while another thread destroys the loop), then frees every
// registration left, unlinking queue registrations from their queues and
// running every due on_removed. After the drain the handle stops resolving. A
// stale, racing or forged handle is a fatal error (CEVENTLOOP_NULL is a no-op),
// and so is a call from a callback on one of the loop's threads. Destroy a
// watched queue only after removing its registration; the queue destroy
// asserts otherwise.
void ceventloop__destroy(ceventloop loop);

// Internal: The cleanup handler of the _scoped macros. NULL does nothing. gcov
// can't count the closing brace of this always-inline function at -O3, hence
// the marker on that one line; don't copy it to a line a test can reach.
static inline __attribute__((always_inline)) void ceventloop__destroy_cleanup(
    ceventloop *lp) {
  if (lp && *lp) {
    ceventloop__destroy(*lp);
    *lp = CEVENTLOOP_NULL;
  }
} /* LCOV_EXCL_LINE */

// Destroys the loop and sets loop to CEVENTLOOP_NULL, so the stale handle
// isn't used again (loop is evaluated once and must be a modifiable lvalue;
// CEVENTLOOP_NULL does nothing). It first shuts the loop down and drains it
// while the handle still works, so a callback that runs meanwhile can remove
// its registrations, close its fd and free its arg. Then it frees every
// registration left, unlinks queue registrations from their queues, runs every
// due on_removed, and waits for every resolved use of the handle to finish. A
// stale or forged handle, a concurrent destroy of the same handle, and a call
// from one of the loop's own callbacks are fatal errors. Destroy a watched
// queue only after you remove its registration.
#define ceventloop_destroy(loop) \
  ceventloop__destroy_impl(loop, \
                           ccol__uniq(ceventloop__destroy_slot, __COUNTER__))

// Internal: The body of ceventloop_destroy(), like ccircq__destroy_impl().
#define ceventloop__destroy_impl(loop, slot) \
  do {                                       \
    __typeof__(loop) *slot = &(loop);        \
    ceventloop__destroy(*slot);              \
    *slot = CEVENTLOOP_NULL;                 \
  } while (0)

// Declares an uninitialized loop handle variable called name, for a loop that
// you create later with ceventloop_create() and destroy yourself with
// ceventloop_destroy().
#define ceventloop_declare(name) ceventloop name

// Declares a loop handle variable called name that starts at CEVENTLOOP_NULL
// and is destroyed when it goes out of scope, so no exit path can leak the
// loop. Create the loop into it later with ceventloop_create().
#define ceventloop_declare_scoped(name)                           \
  ceventloop name ccol__destructor(ceventloop__destroy_cleanup) = \
      CEVENTLOOP_NULL

// Declares a loop handle variable called name and creates the loop with the
// default allocator, for code that can't go on without its loop: A failed
// create stops the program with a fatal error that names the variable and the
// reason. max_events_per_wait (1 to INT_MAX) sizes the poller's batch,
// num_lock_stripes (at least 1) is the number of registry locks, and
// num_reactor_threads (at least 1) is the total thread count. You destroy the
// loop with ceventloop_destroy(), after removing every registration. For
// example ceventloop_construct(loop, 32, 1, 1).
#define ceventloop_construct(name, max_events_per_wait, num_lock_stripes,   \
                             num_reactor_threads)                           \
  ceventloop name = CEVENTLOOP_NULL;                                        \
  do {                                                                      \
    char *ceventloop__err = NULL;                                           \
    (name) = ceventloop_create((max_events_per_wait), (num_lock_stripes),   \
                               (num_reactor_threads), &ceventloop__err);    \
    if (!(name)) {                                                          \
      ccol__fatal_err("ceventloop_construct('%s'): %s", #name,              \
                      ceventloop__err ? ceventloop__err : "unknown error"); \
    }                                                                       \
  } while (0)

// Declares a loop handle variable called name and creates the loop with the
// default allocator; a failed create stops the program with a fatal error that
// names the variable and the reason. The loop is destroyed when name goes out
// of scope, so no exit path can leak it; remove its registrations first.
// max_events_per_wait (1 to INT_MAX) sizes the poller's batch,
// num_lock_stripes (at least 1) is the number of registry locks, and
// num_reactor_threads (at least 1) is the total thread count.
#define ceventloop_construct_scoped(name, max_events_per_wait,              \
                                    num_lock_stripes, num_reactor_threads)  \
  ceventloop name ccol__destructor(ceventloop__destroy_cleanup) =           \
      CEVENTLOOP_NULL;                                                      \
  do {                                                                      \
    char *ceventloop__err = NULL;                                           \
    (name) = ceventloop_create((max_events_per_wait), (num_lock_stripes),   \
                               (num_reactor_threads), &ceventloop__err);    \
    if (!(name)) {                                                          \
      ccol__fatal_err("ceventloop_construct_scoped('%s'): %s", #name,       \
                      ceventloop__err ? ceventloop__err : "unknown error"); \
    }                                                                       \
  } while (0)

#pragma GCC visibility pop
