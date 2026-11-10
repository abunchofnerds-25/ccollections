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
// between an owner thread and a worker thread. Nothing is copied: a send hands
// over the ownership of a pointer. cselect() waits on several queues and fds,
// and ceventloop is a reactor that dispatches their readiness to callbacks.
#pragma GCC visibility push(default)

// Opaque handles of the three kinds of queue.
typedef struct ccircq ccircq;

typedef struct cdynmq cdynmq;

typedef struct cchan cchan;

// A message. A successful send sets the sender's data to NULL and the receiver
// owns (and frees) it. data NULL with size 0 is a valid sentinel message; data
// NULL with a size, or a pointer with size 0, is refused.
typedef struct cmessage {
  void *data;
  size_t size;
} cmessage;

// Creates a circular queue of at most max_size messages (1 to
// SIZE_MAX / sizeof(cmessage), since the queue is one array). mmgmt_procs is
// your allocator, or NULL for malloc and free. On failure it gives NULL and an
// error string in *err_str when err_str isn't NULL. Destroy it with
// ccircq_destroy().
ccircq *ccircq_create_mp(size_t max_size, ccol_memmgmt_procs *mmgmt_procs,
                         char **err_str);

// ccircq_create_mp() with the default allocator, for a caller with no
// allocator of its own. It evaluates to the queue or NULL.
#define ccircq_create(max_size, err_str) \
  ccircq_create_mp((max_size), NULL, (err_str))

// Internal: destroys cq; ccircq_destroy() calls it and then clears the pointer.
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

// Internal: the body of ccircq_destroy(), which calls ccircq__destroy(*slot)
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

// Like ccircq_send(), for a producer that must never block: it gives
// ccol_container_full when the queue is full.
ccol_retval ccircq_try_send(ccircq *cq, cmessage *msg);

// Like ccircq_send(), for a producer that may wait but not for ever: it waits
// at most timeout_us microseconds on CLOCK_MONOTONIC, so a wall clock step
// doesn't change it. 0 does exactly what ccircq_try_send() does, and a value
// too large for a deadline (UINT64_MAX) saturates, which is a wait with no end.
// It also gives ccol_timed_out, or ccol_unexpected_failure with errno set when
// the wait itself fails.
ccol_retval ccircq_timed_send(ccircq *cq, cmessage *msg, uint64_t timeout_us);

// Receives a message into target_buf, for a consumer with nothing else to do:
// it waits with no time limit, and you then own target_buf->data. It gives
// ccol_success or ccol_invalid_args. Turning sends off doesn't wake it.
ccol_retval ccircq_recv(ccircq *cq, cmessage *target_buf);

// Like ccircq_recv(), for a consumer that must not block (after cselect(), or
// in a loop callback): it gives ccol_container_empty instead of waiting.
ccol_retval ccircq_try_recv(ccircq *cq, cmessage *target_buf);

// Like ccircq_recv(), for a consumer that also has other work: it waits at
// most timeout_us microseconds, with the rules of ccircq_timed_send(). 0 acts
// as ccircq_try_recv(), and it can give ccol_timed_out or
// ccol_unexpected_failure.
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
// never block: its send fails only for lack of memory or at ccol_max_elem_count
// messages. The other arguments and the failure report are as for
// ccircq_create_mp(). Destroy it with cdynmq_destroy().
cdynmq *cdynmq_create_mp(ccol_memmgmt_procs *mmgmt_procs, char **err_str);

// cdynmq_create_mp() with the default allocator. It evaluates to the queue or
// NULL.
#define cdynmq_create(err_str) cdynmq_create_mp(NULL, (err_str))

// Internal: destroys dq; cdynmq_destroy() calls it and then clears the pointer.
// Use the macro instead.
void cdynmq__destroy(cdynmq *dq);

// Destroys the queue and its nodes and sets dq to NULL (dq is evaluated once),
// with every rule of ccircq_destroy(): it asserts on a queue that isn't empty
// or is still watched, waits for callbacks already running, and is fatal from a
// callback that dispatches this queue.
#define cdynmq_destroy(dq) \
  cdynmq__destroy_impl(dq, ccol__uniq(ccol__dq_destroy_slot, __COUNTER__))

// Internal: the body of cdynmq_destroy(), like ccircq__destroy_impl().
#define cdynmq__destroy_impl(dq, slot) \
  do {                                 \
    __typeof__(dq) *slot = &(dq);      \
    cdynmq__destroy(*slot);            \
    *slot = NULL;                      \
  } while (0)

// Appends msg without blocking, the only send an unbounded queue needs. The
// queue owns msg->data on success and you keep it on failure. It gives
// ccol_success, ccol_invalid_args, ccol_not_permitted when sends are off,
// ccol_container_full at ccol_max_elem_count messages, or
// ccol_not_enough_memory.
ccol_retval cdynmq_send(cdynmq *dq, cmessage *msg);

// Receives the message at the head into target_buf, for a consumer with
// nothing else to do: it waits with no time limit, and you then own
// target_buf->data. Turning sends off doesn't wake it.
ccol_retval cdynmq_recv(cdynmq *dq, cmessage *target_buf);

// Like cdynmq_recv(), for a consumer that must not block: it gives
// ccol_container_empty instead of waiting.
ccol_retval cdynmq_try_recv(cdynmq *dq, cmessage *target_buf);

// Like cdynmq_recv(), for a consumer that also has other work: it waits at
// most timeout_us microseconds, with the rules of ccircq_timed_recv().
ccol_retval cdynmq_timed_recv(cdynmq *dq, cmessage *target_buf,
                              uint64_t timeout_us);

// Turns sends off, so an owner can stop new work. No sender ever blocks on a
// dynamic queue, so it only affects later sends; the write readiness rule of
// ccircq_disable_sending() applies. Gives ccol_success or ccol_invalid_args.
ccol_retval cdynmq_disable_sending(cdynmq *dq);

// Turns sends back on after cdynmq_disable_sending(). Gives ccol_success or
// ccol_invalid_args.
ccol_retval cdynmq_enable_sending(cdynmq *dq);

// Gives a snapshot of the number of queued messages, or (size_t)-1 for a NULL
// queue.
size_t cdynmq_msg_count(cdynmq *dq);

// Creates a cchan, for two-way traffic between an owner thread and its
// workers: two circular queues of at most max_size messages each,
// cchan_owner_to_workers and cchan_workers_to_owner. The calling thread becomes
// the owner and every other thread is a worker; each call picks its direction
// from the caller's thread ID, so no call names one. The owner must stay alive
// while the channel is in use, since the OS can give its ID to a new thread,
// which would then be routed as the owner. Other arguments as for
// ccircq_create_mp(). Destroy it with cchan_destroy().
cchan *cchan_create_mp(size_t max_size, ccol_memmgmt_procs *mmgmt_procs,
                       char **err_str);

// cchan_create_mp() with the default allocator. It evaluates to the channel or
// NULL.
#define cchan_create(max_size, err_str) \
  cchan_create_mp((max_size), NULL, (err_str))

// Internal: destroys ch; cchan_destroy() calls it and then clears the pointer.
// Use the macro instead.
void cchan__destroy(cchan *ch);

// Destroys the channel and both its queues and sets ch to NULL (ch is
// evaluated once), with every rule of ccircq_destroy() applied to each
// direction: drain both and remove every registration first, and never call it
// from a callback that dispatches one of its directions.
#define cchan_destroy(ch) \
  cchan__destroy_impl(ch, ccol__uniq(cchan__destroy_slot, __COUNTER__))

// Internal: the body of cchan_destroy(), like ccircq__destroy_impl().
#define cchan__destroy_impl(ch, slot) \
  do {                                \
    __typeof__(ch) *slot = &(ch);     \
    cchan__destroy(*slot);            \
    *slot = NULL;                     \
  } while (0)

// The channel versions of the ccircq send and receive calls, with the same
// uses and results, so owner and workers need no direction argument. The owner
// sends to cchan_owner_to_workers and receives from cchan_workers_to_owner, and
// a worker the other way around.
ccol_retval cchan_send(cchan *ch, cmessage *msg);

ccol_retval cchan_try_send(cchan *ch, cmessage *msg);

ccol_retval cchan_timed_send(cchan *ch, cmessage *msg, uint64_t timeout_us);

ccol_retval cchan_recv(cchan *ch, cmessage *target_buf);

ccol_retval cchan_try_recv(cchan *ch, cmessage *target_buf);

ccol_retval cchan_timed_recv(cchan *ch, cmessage *target_buf,
                             uint64_t timeout_us);

// The two directions of a cchan, for the control calls below.
typedef enum cchan_direction {
  cchan_owner_to_workers = 0,
  cchan_workers_to_owner
} cchan_direction;

// Turns sends on direction d off or back on, so one side can stop new work, as
// ccircq_disable_sending() and ccircq_enable_sending() do; the other direction
// isn't touched. Both give ccol_invalid_args for a NULL ch or a bad d.
ccol_retval cchan_disable_sending(cchan *ch, cchan_direction d);

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
// receive. cselect_write waits until a send doesn't have to wait: the queue has
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

// Build a selectable from a ccircq or a cdynmq pointer and a direction, for
// cselect() or ceventloop_add(). Each evaluates to a cselectable value.
#define ccircq_selectable(q_, sel_dir) \
  ((cselectable){.type = cselectable_type_ccircq, .dir = (sel_dir), .cq = (q_)})

#define cdynmq_selectable(q_, sel_dir) \
  ((cselectable){.type = cselectable_type_cdynmq, .dir = (sel_dir), .dq = (q_)})

// Build a selectable from an fd and a direction, so cselect() and the loop can
// watch fds beside queues. It evaluates to a cselectable value. cselect_read
// watches EPOLLIN and cselect_write EPOLLOUT; both also get EPOLLERR and
// EPOLLHUP, and a read selectable EPOLLRDHUP. The library only reports
// readiness; you do the read or write on the fd.
#define cselectable_fd(fd_, sel_dir) \
  ((cselectable){.type = cselectable_type_fd, .dir = (sel_dir), .fd = (fd_)})

// Gives a selectable for the queue of ch that the calling thread reads from
// (cselect_read, like cchan_recv()) or writes to (cselect_write, like
// cchan_send()). The queue is resolved once, here, and the selectable keeps
// watching it whichever thread waits on it later. In an event loop callback,
// receive or send with ccircq_try_recv() or ccircq_try_send() on sel.cq, never
// a cchan_ call: the loop's threads count as workers, so a cchan_ call there
// can pick the other queue. A NULL ch gives a selectable that cselect()
// refuses.
cselectable cchan_selectable(cchan *ch, cselect_dir dir);

// Waits until one of the n selectables is ready for its own direction and sets
// *ready_index; the caller then makes its own non-blocking receive, send, read
// or write and checks its result. An array may mix directions, queues and fds.
// Ready selectables share the calls fairly: each call scans in rotation order
// from a start that moves per call, so a selectable that stays ready wins its
// share. Which one a call reports isn't specified. Each producer action wakes
// exactly one waiter, and a waiter that leaves without acting hands the wake
// on. n goes from 1 to INT_MAX. It gives ccol_success, ccol_invalid_args (a
// NULL or zero argument, a NULL queue, a negative fd, an unknown type or dir),
// ccol_not_enough_memory (nothing linked stays linked, no message moves), or
// ccol_unexpected_failure when a system call or a primitive init fails. Without
// fds it uses only condition variables. With fds it first asks poll(2) without
// waiting and uses epoll (kqueue on the BSDs and macOS) only when it must wait,
// so every fd must be something epoll can watch: a regular file or a directory
// gives ccol_unexpected_failure. Same as cselect_timed() with UINT64_MAX.
ccol_retval cselect(size_t *ready_index, size_t n, cselectable *selectables);

// cselect() with a limit of timeout_us microseconds on CLOCK_MONOTONIC, after
// which it gives ccol_timed_out and leaves *ready_index alone. UINT64_MAX waits
// with no limit, and 0 polls once, still reporting what is ready at the moment
// of the call. Any other value waits at least that long, even past the INT_MAX
// milliseconds of one kernel wait, and looks once more at every selectable
// before it reports a timeout. It also gives ccol_unexpected_failure when the
// timed condition wait fails.
ccol_retval cselect_timed(size_t *ready_index, size_t n,
                          cselectable *selectables, uint64_t timeout_us);

// cselect() and cselect_timed() with the selectables in line, for a fixed set
// written at the call site, for example cselect_va(&idx,
// ccircq_selectable(q0, cselect_read), cchan_selectable(ch, cselect_read)).
// They evaluate to the ccol_retval of the call, and each argument is evaluated
// once. They use a statement expression, so they need GNU C.
#define cselect_va(ready_index, ...)                                       \
  __extension__({                                                          \
    cselectable cselect__arr[] = {__VA_ARGS__};                            \
    cselect((ready_index), sizeof(cselect__arr) / sizeof(cselect__arr[0]), \
            cselect__arr);                                                 \
  })

#define cselect_timed_va(ready_index, timeout_us, ...)            \
  __extension__({                                                 \
    cselectable cselect__arr[] = {__VA_ARGS__};                   \
    cselect_timed((ready_index),                                  \
                  sizeof(cselect__arr) / sizeof(cselect__arr[0]), \
                  cselect__arr, (timeout_us));                    \
  })

// A long-lived reactor. Exactly one poller thread waits on epoll (kqueue on the
// BSDs and macOS), on purpose: many threads waiting on one epoll set all wake
// for one event, and EPOLLEXCLUSIVE doesn't help with that. With
// num_reactor_threads > 1 the callbacks run on a separate pool of dispatch
// workers, and two things still hold. The callbacks of one registration never
// run at the same time, and neither do the read and the write registration of
// one fd, so state shared across both directions of an fd needs no lock. And a
// removed registration's fd number can be reused at once: a dispatch checks
// liveness right before it calls a callback, so a stale dispatch does nothing,
// and the loop never collects a second dispatch for an entry that has one in
// flight, which makes ceventloop_modify from a callback safe. In every
// configuration the calls that take a ceventloop_reg resolve it through a
// generation-tagged slot table first, so threads may race them on one handle,
// or use it long after a remove, and the loser just gets ccol_invalid_args (or
// generation 0). Like cselect(), the loop only reports readiness; your callback
// does the receive or the send.
typedef struct ceventloop__s ceventloop__s;

// The loop handle: a slot index and a generation packed into a value. It isn't
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
// ccol_not_permitted). The race is the same as for the write side of cselect().
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
// that an fd died. Conditions are level-triggered: the loop keeps reporting one
// until it clears or you remove or pause the registration, so a handler that
// reads an end of file must remove the registration (before closing the fd). On
// the BSDs and macOS an error-only registration may hear each condition only
// once.
typedef void (*ceventloop_error_fn)(ceventloop loop, ceventloop_reg reg,
                                    cselectable *sel, void *arg);

// Runs exactly once for every registration that was really added, when the
// loop can prove that you may free arg: after a ceventloop_remove (from any
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
// batch buffer. It gives CEVENTLOOP_NULL on bad arguments, a failed allocation
// or system call, when the process has no thread-specific key left for the two
// keys every loop needs (created once per process), and after the library's
// exit destructors have released the slot table.
ceventloop ceventloop_create_mp(size_t max_events_per_wait,
                                size_t num_lock_stripes,
                                size_t num_reactor_threads,
                                ccol_memmgmt_procs *mmgmt_procs,
                                char **err_str);

// ceventloop_create_mp() with the default allocator. It evaluates to the handle
// or CEVENTLOOP_NULL.
#define ceventloop_create(max_events_per_wait, num_lock_stripes,  \
                          num_reactor_threads, err_str)           \
  ceventloop_create_mp((max_events_per_wait), (num_lock_stripes), \
                       (num_reactor_threads), NULL, (err_str))

// Registers sel with the loop for one direction, sel.dir, so its readiness
// reaches your callbacks, and gives the handle. For both directions of one fd,
// add twice; to change direction over time, use ceventloop_modify. A second
// registration for a direction already in use on the same fd is refused. Queues
// have no such limit: any number of registrations, in any loops, and cselect()
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
// that stopped being watched (see ceventloop_handlers), even when new_dir is
// the current direction. It gives ccol_success, ccol_invalid_args for a bad or
// stale loop or reg, a queue registration or a bad new_dir, and
// ccol_not_permitted when another registration on the fd holds new_dir.
// Thread-safe, also from a callback.
ccol_retval ceventloop_modify(ceventloop loop, ceventloop_reg reg,
                              cselect_dir new_dir);

// Stops event delivery for an fd registration and keeps it whole: its slot,
// its count in ceventloop_reg_count and its generation stay, and only the fd's
// interest mask leaves it out. No callback runs for it while paused; the other
// direction of the fd isn't touched. It's the cheap form of a remove followed
// by an add (no allocation, no registry churn, one epoll_ctl on one thread).
// Pausing a paused registration succeeds. Results as for ceventloop_modify()
// without ccol_not_permitted.
ccol_retval ceventloop_pause(ceventloop loop, ceventloop_reg reg);

// Puts a paused registration back into the fd's interest mask, and re-arms one
// that stopped being watched. Resuming anything else succeeds and does nothing.
// It gives ccol_invalid_args for a bad or stale loop or reg (the connection may
// have closed meanwhile) or a queue registration.
ccol_retval ceventloop_resume(ceventloop loop, ceventloop_reg reg);

// Removes a registration. It may run from the registration's own callback,
// from any thread, and during a dispatch of it. After it returns the loop calls
// none of its callbacks again, collected ones included, but it doesn't wait for
// a dispatch already running: to free an arg that a callback uses, wait for
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

// Internal: destroys the loop; ceventloop_destroy() calls it and then clears
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

// Internal: the cleanup handler of the _scoped macros. NULL does nothing. gcov
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
// CEVENTLOOP_NULL is fine). It waits for every resolved use of the handle to
// finish. A concurrent call on the same handle, and a call from one of the
// loop's own callbacks, are fatal errors.
#define ceventloop_destroy(loop) \
  ceventloop__destroy_impl(loop, \
                           ccol__uniq(ceventloop__destroy_slot, __COUNTER__))

// Internal: the body of ceventloop_destroy(), like ccircq__destroy_impl().
#define ceventloop__destroy_impl(loop, slot) \
  do {                                       \
    __typeof__(loop) *slot = &(loop);        \
    ceventloop__destroy(*slot);              \
    *slot = CEVENTLOOP_NULL;                 \
  } while (0)

// Declares a loop handle named name, to be created later; the _scoped form
// starts at CEVENTLOOP_NULL and destroys the loop at the end of the scope.
#define ceventloop_declare(name) ceventloop name

#define ceventloop_declare_scoped(name)                           \
  ceventloop name ccol__destructor(ceventloop__destroy_cleanup) = \
      CEVENTLOOP_NULL

// Declares a loop named name and creates it with ceventloop_create(), stopping
// the program with ccol__fatal_err() on failure, for code that can't go on
// without its loop. The _scoped form also destroys it at the end of the scope.
// For example ceventloop_construct(loop, 32, 1, 1), then add, remove every
// registration, and ceventloop_destroy(loop).
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
