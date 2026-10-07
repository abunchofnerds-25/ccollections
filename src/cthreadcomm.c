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
#include <cthreadpool.h>
#include <cvector.h>
#include <errno.h>
#include <internal/cdeadline.h>
#include <internal/cpoll.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef RUNNING_UNIT_TESTS
#include <assert.h>
#endif

/* Initialises cond. Every absolute deadline that goes to
 * ccol_cond_var_timedwait on this condition variable is then measured
 * against CLOCK_MONOTONIC. The default clock is CLOCK_REALTIME. Returns 0
 * on success. If there is a failure, it returns the first non-zero status
 * that the threading library reports.
 *
 * This function initialises every condition variable in this file that a
 * timed wait can block on. Every deadline for one of those waits comes from
 * CLOCK_MONOTONIC. This is why the two can never disagree about the clock
 * of a deadline. A user can set CLOCK_REALTIME. An administrator, an NTP
 * step or a resume of a virtual machine can move it backwards at any
 * moment. A deadline against that clock then sits further in the future. A
 * ccol_circq_timed_recv_zc with a 100 ms timeout then blocks for the whole
 * duration of the backward step. Nothing can step CLOCK_MONOTONIC, so the
 * caller gets the timeout that it asked for. An untimed ccol_cond_var_wait
 * ignores the clock attribute, so this costs the untimed waiters nothing. */
static int _init_monotonic_cond_var(ccol_cond_var_t *cond) {
  ccol_cond_var_attr_t attr;
  int rv = ccol_cond_var_attr_init(attr);
  if (rv != 0) return rv;
  rv = ccol_cond_var_attr_setclock(attr, CLOCK_MONOTONIC);
  if (rv == 0) rv = ccol_cond_var_init_ca(*cond, attr);
  ccol_cond_var_attr_destroy(attr);
  return rv;
}

/* A waiter node. A thread that blocks in ccol_select registers one. Each node
 * stays on the heap for the full duration of that call. The node is on the
 * heap to prevent a stack overflow when n is large. A thread must hold the
 * mutex of the owning queue when it reads or changes the waiter list. This
 * makes sure that these nodes stay valid while a producer walks the list. */
typedef struct ccol_sel_waiter {
  ccol_mutex_t *sel_mtx;
  ccol_cond_var_t *sel_cond;
  bool *ready;
  int efd; /* eventfd for epoll mode; -1 in condvar-only mode */
  /* The number of listeners that the outstanding wake of this node already
   * visited without any of them moving a message. A wake that a send, a
   * receive or any other fresh event delivers carries 0. Only a
   * ccol_event_loop registration reads it, when it consumes its wake; see
   * _ccol_event_loop_queue_cascade_notify_next. Guarded by *sel_mtx. It sits
   * in the padding after efd, so the node keeps its size. */
  uint32_t fwd_hops;
  struct ccol_sel_waiter *prev;
  struct ccol_sel_waiter *next;
} ccol_sel_waiter;

#ifdef RUNNING_UNIT_TESTS
/* Test-only. One of _sendto_cq, _recvfrom_cq, ccol_circq_disable_sending or
 * ccol_circq_enable_sending calls this function while it holds the mutex of
 * the queue. This delay widens the window between that lock and the
 * ccol_mutex_lock(*w->sel_mtx) call a few lines below. That window is
 * normally only a few instructions long. The delay makes it as long as the
 * test needs. A test can then land the atfork prepare() handler of a
 * concurrent fork() call inside this exact window every time. This
 * reproduces the lock-order hazard between the queue mutex and wait_mtx
 * that the doc comment of _cthreadcomm_atfork_prepare describes. Without
 * the delay, the test depends on timing luck, because the window in
 * production is far too narrow to hit by chance. See
 * fork_safety.fork_does_not_deadlock_with_queue_registered_before_event_loop
 * in tests/cthreadcomm/tests.c. */
static _Atomic int g_notify_waiter_test_delay_us = 0;
void _notify_waiter_test_set_delay_us(int us) {
  atomic_store(&g_notify_waiter_test_delay_us, us);
}

/* Test-only. Counts the eventfd wakes that _notify_waiter delivers. See
 * ccol_select_test_eventfd_wake_count in cthreadcomm.h. Relaxed ordering is
 * enough: a test reads it only after the threads that bump it have finished
 * the burst that it measures. */
static _Atomic uint64_t g_notify_waiter_eventfd_wakes = 0;
uint64_t ccol_select_test_eventfd_wake_count(void) {
  return atomic_load_explicit(&g_notify_waiter_eventfd_wakes,
                              memory_order_relaxed);
}
#endif

/* The wake descriptors of this file; see ccol_wakefd_create() in
 * internal/cpoll.h. */
static void _eventfd_notify(int efd) { ccol_wakefd_notify(efd); }

static void _eventfd_drain(int efd) { ccol_wakefd_drain(efd); }

/* Wakes one waiter node. The caller must hold the mutex of the owning queue
 * for the whole call, so that the node pointer stays valid.
 *
 * *ready is the one record of whether a wake is outstanding for the node. The
 * first notify after the owner of the node cleared it sets it and delivers
 * the wake: a write to the eventfd of the node when it has one, and a signal
 * of its condition variable otherwise. A node with an eventfd never waits on
 * its condition variable, because both a ccol_select() call in epoll mode and
 * a ccol_event_loop registration wait on the eventfd. Every later notify
 * finds *ready already set and delivers nothing, because the wake that is
 * already outstanding covers it. A queue with an event-loop listener
 * therefore costs one write(2) for each dispatch of that listener, and not
 * one for each message.
 *
 * The eventfd write happens under sel_mtx, together with the store to
 * *ready. The owner clears *ready and drains the eventfd in one critical
 * section under the same mutex; see _queue_reg_consume_wake and the Phase 3
 * step of ccol_select_timed. The flag and the counter of the eventfd can
 * therefore never disagree: *ready is true exactly when a wake is pending on
 * the eventfd. A notify that lands after the clear always finds *ready false
 * and always writes, so no wake is lost.
 *
 * Every wake that this function delivers is a fresh one, so it also sets the
 * hop count of the node to 0; see the fwd_hops field. A fresh wake that
 * merges into a forwarded one therefore restarts the walk that
 * _ccol_event_loop_queue_cascade_notify_next bounds. */
static void _notify_waiter(ccol_sel_waiter *w) {
#ifdef RUNNING_UNIT_TESTS
  int delay_us = atomic_load(&g_notify_waiter_test_delay_us);
  if (delay_us > 0) {
    struct timespec ts = {.tv_sec = delay_us / 1000000,
                          .tv_nsec = (long)(delay_us % 1000000) * 1000L};
    nanosleep(&ts, NULL);
  }
#endif
  ccol_mutex_lock(*w->sel_mtx);
  bool already_pending = *w->ready;
  *w->ready = true;
  w->fwd_hops = 0;
  if (!already_pending && w->efd >= 0) {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&g_notify_waiter_eventfd_wakes, (uint64_t)1,
                              memory_order_relaxed);
#endif
    _eventfd_notify(w->efd);
  }
  ccol_mutex_unlock(*w->sel_mtx);
  if (!already_pending && w->efd < 0) ccol_cond_var_signal(*w->sel_cond);
}

/* Delivers a wake exactly as _notify_waiter does, but one that a listener
 * forwards after a dispatch that moved nothing: hops is the number of
 * listeners that the wake has visited in a row without progress. When a wake
 * is already outstanding for w, the smaller count wins, so a forward never
 * shortens the walk of a wake that is already on its way. The caller holds
 * the mutex of the owning queue. */
static void _notify_waiter_forwarded(ccol_sel_waiter *w, uint32_t hops) {
  ccol_mutex_lock(*w->sel_mtx);
  bool already_pending = *w->ready;
  *w->ready = true;
  if (!already_pending || hops < w->fwd_hops) w->fwd_hops = hops;
  if (!already_pending && w->efd >= 0) {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&g_notify_waiter_eventfd_wakes, (uint64_t)1,
                              memory_order_relaxed);
#endif
    _eventfd_notify(w->efd);
  }
  ccol_mutex_unlock(*w->sel_mtx);
  if (!already_pending && w->efd < 0) ccol_cond_var_signal(*w->sel_cond);
}

/* True when the waiter list that starts at head holds more than k nodes. It
 * stops walking after k + 1 nodes. The caller holds the mutex of the owning
 * queue. */
static bool _sel_list_longer_than(const ccol_sel_waiter *head, uint32_t k) {
  uint32_t seen = 0;
  for (const ccol_sel_waiter *w = head; w != NULL; w = w->next) {
    if (seen == k) return true;
    seen++;
  }
  return false;
}

/* Wakes ONE waiter on the read or write waiter list of a queue. It also
 * rotates *rotor, so that the next call picks a DIFFERENT waiter. Over time
 * the rotation reaches every waiter that is linked in the list. It does not
 * always pick the same one. The code uses this function for message and
 * slot events, where exactly one resource became available. A wake of more
 * than one waiter would cause a thundering herd. The caller must hold the
 * mutex of the queue.
 *
 * *rotor is NULL, which means "start a fresh cycle at *head". Or it points
 * to a node that is still linked in THIS exact list. Every unlink path
 * keeps this invariant. _sel_unlink_waiter_locked can remove the node that
 * IS *rotor. It then sets *rotor to the .next of that node. When the node
 * has no .next, it sets *rotor to NULL. Nothing other than this function
 * and that unlink fixup reads *rotor directly.
 *
 * The rotation is what makes silent, permanent starvation impossible. The
 * other design is to notify *head every time, and that design works badly
 * with the PERMANENTLY linked registrations of ccol_event_loop. The node of
 * a ccol_select() caller is transient. It links again every time that the
 * caller must wait again. It can therefore get a different position in the
 * list. A registration does not. _sel_link_waiter always prepends, so the
 * registration that linked most recently stays *head*. It is then the ONLY
 * one that gets a direct notify for its whole registered lifetime.
 *
 * The "cascade" mechanism (_ccol_event_loop_queue_cascade_notify_next) is
 * no substitute for the rotation. It forwards a wake along the list only
 * when the queue is STILL ready after the callback of the notified waiter
 * returns. A callback that keeps up with the traffic often
 * leaves nothing to forward. One plain ccol_circq_try_recv_zc() for each
 * call, with no loop, is such a callback, and it is an ordinary documented
 * pattern.
 *
 * Without the rotation, one registration takes every notify. Every OTHER
 * live registration on the same queue and the same direction then gets no
 * call at all. This lasts for as long as that one registration stays
 * registered. For example, make two ccol_event_loop_add() calls on the read
 * direction of one ccol_circular_queue. Let both callbacks do one
 * immediate ccol_circq_try_recv_zc() with no drain loop. Feed the queue one
 * message at a time, so that no backlog can build. The second, older
 * registration then gets ZERO callbacks across thousands of sent messages.
 * That contradicts the guarantee of this module that no live listener is
 * ever passed over indefinitely. The doc comment of ccol_event_loop_add
 * states that guarantee.
 *
 * The rotation changes the target waiter on every single notify. It is
 * independent of the cascade and works in addition to it. The cascade
 * depends on readiness and has its own separate purpose: it surfaces a
 * backlog that the target of a single notify can leave stranded. The
 * rotation visits every live waiter on the list once for each full cycle.
 * This is why the callback of a sibling registration cannot keep the queue
 * drained and skip a waiter forever. */
static void notify_one_sel_waiter(ccol_sel_waiter **head,
                                  ccol_sel_waiter **rotor) {
  ccol_sel_waiter *target = *rotor ? *rotor : *head;
  if (!target) return;
  _notify_waiter(target);
  /* This wraps to NULL when the rotation walks off the tail, that is when
   * target->next == NULL. The next call then starts a fresh cycle at *head.
   * *head can be a different node than at the start of this cycle, because
   * new waiters prepend there. That is correct: a full cycle only needs to
   * reach every waiter that is linked NOW. It does not need to keep the
   * same walk order across cycles. */
  *rotor = target->next;
}

/* Wakes every thread that blocks in ccol_select on this queue. The code uses
 * this function only for state-change events: disable_sending and
 * enable_sending. For those events every blocked thread must look at the
 * state again, even when no resource is available. The caller must hold the
 * mutex of the queue. */
static void notify_all_sel_waiters(ccol_sel_waiter *head) {
  for (ccol_sel_waiter *w = head; w != NULL; w = w->next) _notify_waiter(w);
}

/* ========================================================================== */
/*         QUEUE DISPATCH REFERENCES (queue teardown vs. a live callback)     */
/* ========================================================================== */

/* A thread-local key for the whole process. The code creates it on first
 * use. It holds the queue whose ccol_event_loop callback runs on the calling
 * thread now, or NULL when no such callback runs. The key covers the whole
 * process instead of one queue, for the same reason as the dispatch-job key
 * of ccol_event_loop. See the declaration of that key further below. One key
 * answers the question "which callback of which queue runs on me right now"
 * for every queue in the process at once. There is no lifecycle for each
 * queue to manage.
 *
 * live says whether the key exists. The creation can fail, for example in a
 * process that already holds PTHREAD_KEYS_MAX keys. key then names no key of
 * this module; it can name a key that another component owns, and a value set
 * on it would reach the destructor of that component. Only the dispatch of a
 * ccol_event_loop sets the key, and ccol_event_loop_create_with_mprocs refuses
 * to build a loop while live is false, so nothing sets a key that does not
 * exist. A queue destroy reads it only while live is true. The key is deleted
 * when the slot table of ccol_event_loop is released, which happens only once
 * no loop is left; see _release_event_loop_slot_table_locked. A cycle of
 * dlopen() and dlclose() therefore does not use up the keys of the
 * process. */
static struct {
  ccol_thread_ls_key_t key;
  ccol_once_flag_t once;
  atomic_bool live;
} queue_dispatch_marker_bundle = {.once = CCOL_ONCE_INIT};

static void _queue_dispatch_marker_init(void) {
  if (ccol_thread_ls_key_create(queue_dispatch_marker_bundle.key, NULL) == 0)
    atomic_store(&queue_dispatch_marker_bundle.live, true);
}

/* True when the marker key exists. ccol_event_loop_create_with_mprocs asks
 * this before it builds a loop. */
static bool _queue_dispatch_marker_available(void) {
  ccol_call_once(queue_dispatch_marker_bundle.once,
                 _queue_dispatch_marker_init);
  return atomic_load(&queue_dispatch_marker_bundle.live);
}

/* Marks the calling thread: it runs the ccol_event_loop callback of queue.
 * A NULL argument clears the mark. Only the dispatch of a live loop calls
 * this, and a loop exists only after _queue_dispatch_marker_available()
 * answered true, so the key exists here and the call adds no test to the
 * dispatch path. */
static void _queue_dispatch_marker_set(void *queue) {
  ccol_call_once(queue_dispatch_marker_bundle.once,
                 _queue_dispatch_marker_init);
  ccol_thread_ls_set(queue_dispatch_marker_bundle.key, queue);
}

/* Without the key no loop can exist, so no callback of any queue runs on this
 * thread. */
static bool _queue_dispatch_marker_is(const void *queue) {
  if (!_queue_dispatch_marker_available()) return false;
  return ccol_thread_ls_get(queue_dispatch_marker_bundle.key) == queue;
}

/* Blocks until every ccol_event_loop dispatch that still holds a reference to
 * this queue finishes with it. The destroy function of a queue calls this.
 * At that moment the caller already drained the queue and removed every
 * registration that watches it.
 *
 * ccol_event_loop_remove does not wait for a callback that the reactor
 * already collected before that callback ran. A wait there is an AB-BA
 * deadlock against any application lock that a caller can hold across the
 * removal. This is why the removal is asynchronous and reports completion
 * through on_removed. The unlink of the waiter node of the registration
 * makes the queue look unwatched the moment that remove returns. But a
 * dispatch that already started its callback still holds the queue. It holds
 * it through its own collected snapshot, and it is about to call into the
 * queue. Only such a dispatch holds a reference; one that still waits in the
 * queue of the dispatch pool holds none, and it never touches the queue
 * after the removal. See _queue_dispatch_ref_acquire. Without this wait, a
 * free of the queue then is a use-after-free.
 * The ccol_circq_try_recv_zc of the callback then reads a freed queue. Or the
 * reactor blocks forever when it locks a destroyed mutex. That hangs the
 * poller thread and every later ccol_event_loop_destroy that joins it.
 *
 * The wait here is not the thing that remove refuses to do. This function
 * holds no lock at all, because the code unlocks the mutex of the queue
 * before it reaches this function. This is why the function cannot be one
 * side of a lock-order cycle. The dispatch that it waits on takes four
 * locks. Those are the dispatch lock of the entry and the stripe lock of the
 * loop. The other two are the mutex of the queue and the wait mutex of the
 * registration. This thread holds none of them. A caller can still block
 * itself in one way. It can hold an application lock across the destroy
 * that the in-flight callback also needs. The documentation of each destroy
 * function forbids that.
 *
 * A destroy from inside that same callback would wait on itself forever.
 * The thread-local marker above detects that case and makes it fatal. A
 * thread pool refuses a worker that destroys the pool that it runs on in the
 * same way. No resolve or pin scheme catches this case, because a callback
 * reaches the queue through its own call stack, not through a resolve.
 *
 * This function polls, and does not wait for a signal. A signal costs the
 * dispatch path one condition variable signal for each callback. The
 * dispatch path pays that cost for a wait that is cold by design. This poll
 * contends nothing while it sleeps. It reads one atomic and holds no lock.
 * This is why it cannot slow down the dispatch that it waits for. A retry
 * loop that takes a lock again and again would slow that dispatch down. */
static void _queue_wait_for_dispatch_refs(_Atomic size_t *refs,
                                          const void *queue) {
  if (atomic_load(refs) == 0) return;

  if (_queue_dispatch_marker_is(queue)) {
    ccol_fatal_err(
        "a queue must not be destroyed from inside an ccol_event_loop "
        "callback dispatching that same queue (queue=%p)",
        queue);
  }

  while (atomic_load(refs) != 0) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 200000L};
    nanosleep(&ts, NULL);
  }
}

/* ========================================================================== */
/*         QUEUE MUTEX FORK SAFETY (pthread_atfork) registry                  */
/* ========================================================================== */

/* A registry for the whole process. It holds the address of the mutex
 * (ccol_mutex_t*) of every live ccol_circular_queue and of every live
 * ccol_dynamic_queue. The code registers a ccol_at_fork() handler on first
 * use. That handler locks every one of these mutexes before fork() goes on.
 * It unlocks them again immediately after the fork, in the parent and in the
 * child.
 *
 * fork() copies only the calling thread. Some OTHER thread can hold a lock
 * at that moment. Here that lock is a cq->mutex or a dq->mutex. A thread
 * holds it in the middle of a send or a receive. That thread is not the one
 * that calls fork(). The child inherits that lock in the locked state. No
 * thread stays alive in the child that can ever unlock it. Every later
 * operation on that same queue in the child then hangs forever.
 * ccol_circq_send_zc, ccol_circq_recv_zc and ccol_dynmq_send_zc are such
 * operations, because each of them starts with a ccol_mutex_lock(cq->mutex)
 * or a ccol_mutex_lock(dq->mutex). The atfork handling of ccol_event_loop
 * already closes this same hazard for every lock that IT owns. For a
 * queue-backed registration that set includes the wait_mtx of the
 * registration. This registry closes the same gap for the mutex of the
 * queue. The wait_mtx is no substitute for it: wait_mtx only guards the
 * generic ready-flag handshake of _notify_waiter(). msg_count,
 * writing_disabled and the waiter lists all sit behind cq->mutex or
 * dq->mutex. No lock that ccol_event_loop owns covers those, and that is
 * true whether the queue is registered with a loop or not.
 *
 * ccol_channel needs no entry of its own. Two ccol_circular_queue instances
 * back it, and ccol_circular_queue_create_with_mprocs already registers each
 * of them here. Both directions of a ccol_channel are therefore covered
 * automatically.
 *
 * This registry has no generation or handle concept to maintain, unlike
 * ccol_event_loop_slot_table. A ccol_circular_queue and a ccol_dynamic_queue
 * are plain pointers, not opaque handles. The registry is only an unordered
 * bag of live ccol_mutex_t* addresses. The code adds an address at
 * construction and removes it at destruction. The single purpose is to give
 * the merged atfork handler something to walk. See
 * _cthreadcomm_atfork_prepare, which is defined further below, after the
 * full declaration of struct ccol_event_loop_s. This registry needs no fixup
 * that runs only in the child. The child-release path of ccol_event_loop
 * does need one, because it marks foreign_since_fork, replaces its epfd and
 * does more work of that kind. A ccol_circular_queue and a
 * ccol_dynamic_queue own no kernel object and no thread of their own. After
 * the correct unlock of the mutex after the fork, the whole state of the
 * queue is what it was at the fork moment. That state is msg_count, the
 * message array and the waiter lists. It is as safe to use in the child as
 * in the parent. The same reasoning covers every plain condition variable
 * that this module owns, for example read_cond and write_cond here, or
 * joined_cv of ccol_event_loop. Only a LOCK that a now-gone thread holds is
 * a hazard. A condition variable is never "locked" at all, so a fork() needs
 * no work on one.
 *
 * IMPORTANT: the code does NOT give this registry its own independent
 * ccol_at_fork() call for this work. A second, fully separate
 * ccol_at_fork() triple beside the triple of ccol_event_loop is a real
 * deadlock. The prepare handlers of pthread_atfork run in REVERSE
 * registration order. Which of two independent handler sets runs first is
 * therefore only an accident. It depends on which subsystem a process uses
 * first, a ccol_circular_queue or a ccol_event_loop. Take the case where the
 * prepare of ccol_event_loop runs first. It locks the reg->wait_mtx of a
 * queue-backed registration. A separate registry prepare that runs second
 * then locks the cq->mutex of that SAME queue. That is the reverse of the
 * order that every ordinary code path uses. _notify_waiter always locks
 * cq->mutex first and reg->wait_mtx second. ccol_circq_send_zc,
 * ccol_circq_recv_zc and the other such functions call _notify_waiter from a
 * fully unrelated thread that runs at the same time. The cycle is a textbook
 * AB-BA. Create a queue before the first ccol_event_loop of the process.
 * Register that queue with a loop. Let a real ccol_circq_send_zc race a
 * fork() call. The thread that forks then blocks inside fork() itself, while
 * the prepare handler of the registry locks cq->mutex. At the same moment
 * the thread that sends blocks on reg->wait_mtx inside _notify_waiter.
 *
 * The code therefore merges the locking of both subsystems into ONE triple
 * of _cthreadcomm_atfork_prepare, _release and _child_release, defined
 * further below. One single ccol_at_fork() call registers that triple. It
 * locks the mutex of every queue in the one position that agrees with
 * EVERY real nested-lock pattern in this file. That position is after the
 * stripe lock that owns any of the ccol_event_loop registrations of the
 * queue. This matches the stripe->lock before cq->mutex nesting of
 * _ccol_event_loop_add_queue and _ccol_event_loop_remove_unlink. The
 * position is also before the wait_mtx of any of those registrations. This
 * matches the cq->mutex before wait_mtx nesting of _notify_waiter. See the
 * comment of that function for the full three-phase design. The struct of
 * this registry and its add and remove functions are here, close to the code
 * of ccol_circular_queue and ccol_dynamic_queue. The logic that locks across
 * a fork sits beside the code of ccol_event_loop, because it needs the full
 * definition of struct ccol_event_loop_s. */
#if CCOL_FORK_SAFETY_REQUIRED
/* One entry of queue_mutex_registry: the mutex of a live queue, and the
 * address of the registry_idx field of that queue, which always holds the
 * position of this entry in the vector. */
typedef struct {
  ccol_mutex_t *mutex;
  size_t *index;
} _queue_mutex_registry_entry;

static struct {
  ccol_mutex_t mutex;
  ccol_once_flag_t once;
  /* A cvec of _queue_mutex_registry_entry. It has no order. A remove takes
   * the position of its entry from the queue and moves the last entry into
   * the hole, so a remove costs O(1) whatever the number of live queues.
   * NULL once the registry is released at process exit; see
   * _cleanup_queue_mutex_registry. */
  cvec addrs;
  /* Set when the process-exit destructor finds a queue that is still live
   * and leaves the vector alone. The remove that takes out the last address
   * after that then does the release that the destructor could not do. The
   * order of the destructors between translation units is not this library's
   * to decide, and an application destructor can still destroy a queue after
   * this one ran. */
  bool release_deferred;
} queue_mutex_registry = {0};
#endif

/* Guards the ONE shared ccol_at_fork() registration. That registration
 * covers the queue mutexes of this registry and the locks of
 * ccol_event_loop_slot_table. The doc comment of queue_mutex_registry gives
 * the reason for one merged registration instead of two independent ones.
 * This flag is here, and not only where ccol_event_loop_slot_table is, so
 * that _queue_mutex_registry_add and _queue_mutex_registry_remove can
 * trigger the registration on their own. A process can reach those two
 * functions and never touch ccol_event_loop. They therefore must not need
 * ccol_event_loop_slot_table to exist first. */
static ccol_once_flag_t g_cthreadcomm_atfork_once = CCOL_ONCE_INIT;

/* A forward declaration. This function registers the merged ccol_at_fork()
 * triple. Its body is further below, after the full declaration of struct
 * ccol_event_loop_s. The body must come after that declaration, because the
 * merged prepare and release functions that it wires up dereference
 * loop->shutdown_lock, loop->reg_slot_rwlock and loop->stripes[]. The
 * function also initialises the plain data of ccol_event_loop_slot_table on
 * first use: the mutex and both cvecs. It does this through the
 * ccol_call_once of that table. The caller of this function already
 * initialises the plain data of queue_mutex_registry on first use in the
 * same way. BOTH structures are therefore fully ready before the shared
 * ccol_at_fork() handlers can run, whichever subsystem a process uses
 * first. */
static void _cthreadcomm_register_atfork_once(void);

#if CCOL_FORK_SAFETY_REQUIRED
static void _queue_mutex_registry_init_globals(void) {
  if (ccol_mutex_init(queue_mutex_registry.mutex) != 0)
    ccol_fatal_err("queue mutex registry: failed to initialize mutex");
  queue_mutex_registry.addrs =
      cvector_create(sizeof(_queue_mutex_registry_entry), NULL);
  if (!queue_mutex_registry.addrs)
    ccol_fatal_err("queue mutex registry: failed to allocate address vector");
}

/* Registers m, so that a later fork() locks it in
 * _cthreadcomm_atfork_prepare. index is the registry_idx field of the queue
 * that owns m. The registry keeps the position of the entry there, under the
 * registry mutex, for _queue_mutex_registry_remove. The code calls this once
 * for each live queue. It calls it directly after it initialises the mutex of
 * that queue, and after every other field of that queue is fully built. A
 * failure here can therefore unwind like any other allocation failure in that
 * same constructor. Returns false on failure. The one failure is a
 * cvector_push_back call of this registry that cannot grow the vector. The
 * caller must treat that like any other allocation failure at construction
 * time. It must tear down what it already built and report an ordinary,
 * graceful error. It must not abort the process. For the constructor of the
 * queue this is an ordinary out-of-memory case. */
static bool _queue_mutex_registry_add(ccol_mutex_t *m, size_t *index) {
  ccol_call_once(queue_mutex_registry.once, _queue_mutex_registry_init_globals);
  ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  ccol_mutex_lock(queue_mutex_registry.mutex);
  /* A NULL vector means that the process-exit destructor released the
   * registry. A queue built after that cannot be made fork-safe, so its
   * construction fails like an allocation failure. */
  bool ok = false;
  if (queue_mutex_registry.addrs != NULL) {
    _queue_mutex_registry_entry e = {.mutex = m, .index = index};
    *index = cvector_elem_count(queue_mutex_registry.addrs);
    ok = cvector_push_back(queue_mutex_registry.addrs, &e) == ccol_success;
  }
  ccol_mutex_unlock(queue_mutex_registry.mutex);
  return ok;
}

#ifdef RUNNING_UNIT_TESTS
/* Counts the registry entries that _queue_mutex_registry_remove examines. */
static _Atomic uint64_t g_queue_registry_remove_probes_for_tests = 0;
#endif

/* Unregisters m. An earlier _queue_mutex_registry_add(m, index) call with
 * the same index added it. The function reads the position of the entry from
 * *index and moves the current last entry of the registry into the hole,
 * updating the registry_idx of the queue that owns the moved entry. It does
 * not shift every following entry down. The registry is an unordered bag,
 * and _cthreadcomm_atfork_prepare and _release have no contract about the
 * walk order. The function does nothing if the entry at *index is not m.
 * That check is only a defence: every real caller removes exactly once an
 * address that it added itself with success. */
static void _queue_mutex_registry_remove(ccol_mutex_t *m, size_t *index) {
  ccol_call_once(queue_mutex_registry.once, _queue_mutex_registry_init_globals);
  ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  ccol_mutex_lock(queue_mutex_registry.mutex);
  if (!queue_mutex_registry.addrs) {
    ccol_mutex_unlock(queue_mutex_registry.mutex);
    return;
  }
  size_t n = cvector_elem_count(queue_mutex_registry.addrs);
  size_t i = *index;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&g_queue_registry_remove_probes_for_tests,
                            (uint64_t)1, memory_order_relaxed);
#endif
  if (i < n &&
      ((_queue_mutex_registry_entry *)cvector_at(queue_mutex_registry.addrs, i))
              ->mutex == m) {
    _queue_mutex_registry_entry last;
    cvector_pop_back(queue_mutex_registry.addrs, &last);
    if (i != n - 1) {
      *(_queue_mutex_registry_entry *)cvector_at(queue_mutex_registry.addrs,
                                                 i) = last;
      *last.index = i;
    }
  }
  /* The last statement under this lock. It performs the release that the
   * process-exit destructor deferred, once the last live queue is gone. */
  if (queue_mutex_registry.release_deferred &&
      cvector_elem_count(queue_mutex_registry.addrs) == 0) {
    cvector_destroy(queue_mutex_registry.addrs);
    queue_mutex_registry.release_deferred = false;
  }
  ccol_mutex_unlock(queue_mutex_registry.mutex);
}

#ifdef RUNNING_UNIT_TESTS
/* Reports the number of registered queue mutexes in *count and the number of
 * entries that removes examined so far in *remove_probes. Returns true when
 * the registry_idx of the queue behind every entry names the position of
 * that entry. */
bool _ccol_queue_mutex_registry_check_for_tests(size_t *count,
                                                uint64_t *remove_probes) {
  ccol_call_once(queue_mutex_registry.once, _queue_mutex_registry_init_globals);
  ccol_mutex_lock(queue_mutex_registry.mutex);
  size_t n = queue_mutex_registry.addrs
                 ? cvector_elem_count(queue_mutex_registry.addrs)
                 : 0;
  bool ok = true;
  for (size_t k = 0; k < n; k++) {
    _queue_mutex_registry_entry *e = (_queue_mutex_registry_entry *)cvector_at(
        queue_mutex_registry.addrs, k);
    if (*e->index != k) ok = false;
  }
  ccol_mutex_unlock(queue_mutex_registry.mutex);
  if (count) *count = n;
  if (remove_probes)
    *remove_probes = atomic_load(&g_queue_registry_remove_probes_for_tests);
  return ok;
}
#endif

/* Frees the backing array of the registry at process exit. Without this, the
 * --show-leak-kinds=all option of make memtest reports the array as still
 * reachable. This function matches _cleanup_event_loop_slot_table. While a
 * queue is still registered, it only sets release_deferred, and the remove
 * of the last address frees the vector. A queue that an application
 * destructor destroys after this one ran therefore finds the registry
 * intact. cvector_destroy sets the handle to NULL, so a later add or remove
 * sees the release instead of a freed vector. The mutex stays: the merged
 * fork handlers can still take it. */
__attribute__((destructor)) static void _cleanup_queue_mutex_registry(void) {
  ccol_call_once(queue_mutex_registry.once, _queue_mutex_registry_init_globals);
  ccol_mutex_lock(queue_mutex_registry.mutex);
  if (queue_mutex_registry.addrs) {
    if (cvector_elem_count(queue_mutex_registry.addrs) > 0)
      queue_mutex_registry.release_deferred = true;
    else
      cvector_destroy(queue_mutex_registry.addrs);
  }
  ccol_mutex_unlock(queue_mutex_registry.mutex);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* Stores the error that a condition-variable wait returned in errno. The
 * wait reports its error as a return value and leaves errno alone, and the
 * documented contract of the timed send and receive functions is that errno
 * holds the cause of ccol_unexpected_failure. The caller calls this before
 * it unlocks the queue mutex, which never changes errno. Out of line, the
 * error code needs no callee-saved register in the timed functions, so their
 * common path keeps its shape. */
static __attribute__((noinline, cold)) void _ccol_store_wait_errno(int err) {
  errno = err;
}

struct ccol_circular_queue {
  ccol_mutex_t mutex;
  ccol_cond_var_t read_cond;
  ccol_cond_var_t write_cond;

  /* How many threads block on the condition variables above. The waiters
   * themselves keep these counts, and they hold the mutex when they do. The
   * code sends a signal only when the relevant count is not zero.
   *
   * This is a throughput property, not a correctness one. A signal with no
   * waiter still costs a call into the threading library. It also costs a
   * read of the shared state of the condition variable, which its waiters
   * write. The gate removes both from the common path of every send and of
   * every receive. Those callers already share the mutex, and this saves
   * work on top of that. The gate must never leave a real waiter invisible.
   * This is why every wait site sits between the increment and the decrement
   * of these counts. That includes the timed wait sites. */
  size_t readers_waiting;
  size_t writers_waiting;

  size_t read_index;
  size_t write_index;
  size_t max_size;
  size_t msg_count;

  ccol_memmgmt_procs_t *m_procs;

  c_message_t *msg_array;
  bool writing_disabled;

  ccol_sel_waiter *sel_read_waiters_head;
  ccol_sel_waiter *sel_write_waiters_head;

  /* Round-robin cursors for notify_one_sel_waiter. See the doc comment of
   * that function. NULL means "start a fresh cycle at the matching head". */
  ccol_sel_waiter *sel_read_rotor;
  ccol_sel_waiter *sel_write_rotor;

  /* How many ccol_event_loop dispatches hold a reference to this queue now.
   * The reactor takes a reference when it collects a callback whose
   * selectable names this queue. It frees that reference after the callback
   * and its cascade step both finish. __ccol_circular_queue_destroy waits
   * for this count to reach zero before it frees anything. See
   * _queue_wait_for_dispatch_refs. This field is last among these fields on
   * purpose. It is well clear of the fields that the send and receive paths
   * read, and neither path touches it. */
  _Atomic size_t dispatch_refs;

#if CCOL_FORK_SAFETY_REQUIRED
  /* The position of the entry of this queue in queue_mutex_registry. Only
   * the registry reads or writes it, under the registry mutex. It sits after
   * every field of the send and receive paths. */
  size_t registry_idx;
#endif
};

/* Validates the arguments for the creation of a ccol_circular_queue. max_size
 * must be positive and not more than ccol_max_elem_count. If the caller gives
 * a custom allocator, that allocator must be well formed.
 */
static bool verify_circular_queue_create_inputs(
    size_t max_size, ccol_memmgmt_procs_t *mmgmt_procs, char **err_str) {
  if (max_size == 0) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("max_size should be positive");
    }
    return false;
  }

  if (max_size > ccol_max_elem_count) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("max_size can not exceed ccol_max_elem_count");
    }
    return false;
  }

  /* ccol_circular_queue_create_with_mprocs backs the queue with one array of
   * max_size * sizeof(c_message_t) bytes. A plain multiplication computes
   * that size. The code does not use an allocator that checks for overflow,
   * such as calloc. Without this guard, a max_size in about
   * [ccol_max_elem_count / 16, ccol_max_elem_count] makes that
   * multiplication wrap size_t. That range is the top slice of the range
   * that the checks above accept. The allocator then gets a tiny request, or
   * even a request for zero bytes. But the queue still believes that it has
   * room for max_size messages. The first send then corrupts the heap.
   * cvector.c, csort.c and cmempool.c already use the same
   * SIZE_MAX / element_size guard for the same class of allocation. */
  if (max_size > SIZE_MAX / sizeof(c_message_t)) {
    if (err_str) {
      *err_str = CCOL_ERR_STR(
          "max_size is too large: max_size * sizeof(c_message_t) would "
          "overflow size_t");
    }
    return false;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return false;
  }

  return true;
}

/* Allocates and initialises a bounded circular queue with a message array of
 * a fixed size. This function initialises the mutex and both condition
 * variables. At the start, the write direction of the queue is turned on. */
ccol_circular_queue *ccol_circular_queue_create_with_mprocs(
    size_t max_size, ccol_memmgmt_procs_t *mmgmt_procs, char **err_str) {
  if (!verify_circular_queue_create_inputs(max_size, mmgmt_procs, err_str)) {
    return NULL;
  }

  ccol_circular_queue *cq = (ccol_circular_queue *)_ccol_mem_alloc(
      mmgmt_procs, sizeof(ccol_circular_queue));
  if (!cq) {
    if (err_str) {
      *err_str =
          CCOL_ERR_STR("Failed to allocate memory for ccol_circular_queue");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(cq, mmgmt_procs, err_str)) {
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }

  cq->msg_array = (c_message_t *)_ccol_mem_alloc(
      mmgmt_procs, max_size * sizeof(c_message_t));
  if (!cq->msg_array) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("Failed to allocate memory for cq msg_array");
    }
    _ccol_mem_free(mmgmt_procs, cq->m_procs);
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }

  if (ccol_mutex_init(cq->mutex) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to initialize cq mutex");
    _ccol_mem_free(mmgmt_procs, cq->msg_array);
    _ccol_mem_free(mmgmt_procs, cq->m_procs);
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }
  if (_init_monotonic_cond_var(&cq->read_cond) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to initialize cq read_cond");
    ccol_mutex_destroy(cq->mutex);
    _ccol_mem_free(mmgmt_procs, cq->msg_array);
    _ccol_mem_free(mmgmt_procs, cq->m_procs);
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }
  if (_init_monotonic_cond_var(&cq->write_cond) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to initialize cq write_cond");
    ccol_mutex_destroy(cq->mutex);
    ccol_cond_var_destroy(cq->read_cond);
    _ccol_mem_free(mmgmt_procs, cq->msg_array);
    _ccol_mem_free(mmgmt_procs, cq->m_procs);
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }
  cq->read_index = 0;
  cq->write_index = 0;
  cq->max_size = max_size;
  cq->msg_count = 0;
  /* The code sets these two fields explicitly, like every other field here.
   * The struct comes from an allocation that does not zero the memory. The
   * code sends a signal only when one of these fields is not zero.
   * Without this, the common case is a stray non-zero value, which costs a
   * signal that no thread waits for. The rare case is a value whose
   * increment wraps to zero, and then nothing ever wakes a real waiter. */
  cq->readers_waiting = 0;
  cq->writers_waiting = 0;
  cq->writing_disabled = false;
  cq->sel_read_waiters_head = NULL;
  cq->sel_write_waiters_head = NULL;
  cq->sel_read_rotor = NULL;
  cq->sel_write_rotor = NULL;
  atomic_init(&cq->dispatch_refs, (size_t)0);

#if CCOL_FORK_SAFETY_REQUIRED
  /* This registration is the LAST step, after cq is fully built. A failure
   * here can therefore unwind like any earlier allocation failure in this
   * same function. See the comment of _queue_mutex_registry_add for the
   * reason why this failure is graceful and not fatal. */
  if (!_queue_mutex_registry_add(&cq->mutex, &cq->registry_idx)) {
    if (err_str) {
      *err_str = CCOL_ERR_STR(
          "Failed to register ccol_circular_queue's mutex for fork safety");
    }
    ccol_mutex_destroy(cq->mutex);
    ccol_cond_var_destroy(cq->read_cond);
    ccol_cond_var_destroy(cq->write_cond);
    _ccol_mem_free(mmgmt_procs, cq->msg_array);
    _ccol_mem_free(mmgmt_procs, cq->m_procs);
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }
#endif

  if (err_str) {
    *err_str = NULL;
  }

  return cq;
}

/* Destroys the circular queue. The function asserts in two cases. The first
 * case is a queue that still holds a message that nobody consumed, because
 * the data pointer of that message would leak. The second case is a waiter of
 * ccol_select() or of ccol_event_loop that is still linked into one of the
 * waiter lists. The second case is a real use-after-free hazard, not only a
 * leak. The unlink of a linked node goes through the queue that holds the
 * list, so the deregister step reads cq->sel_*_waiters_head, cq->sel_*_rotor
 * and cq->mutex. The caller must call ccol_event_loop_remove() first, or let
 * ccol_select() return first. A destroy of cq before that leaves the next
 * touch of that node to dereference freed queue memory. That next touch comes
 * from ccol_event_loop_remove, from the teardown walk of
 * __ccol_event_loop_destroy, or from the Phase 3 deregister of
 * ccol_select_timed. Both cases are bugs in the caller. This function must
 * make them visible before it frees the memory, so that they do not appear
 * later as corruption. */
void __ccol_circular_queue_destroy(ccol_circular_queue *cq) {
  if (cq) {
    /* This read happens under the mutex. Phase 1 or Phase 3 of a concurrent
     * ccol_select() call can link or unlink a node in either list at this
     * exact moment. The notify_one_sel_waiter call of a producer or of a
     * consumer can do the same. The read must therefore be race-free. An
     * unlocked peek at fields that a different thread can update is not
     * enough. */
    ccol_mutex_lock(cq->mutex);
    bool has_sel_waiters = (cq->sel_read_waiters_head != NULL) ||
                           (cq->sel_write_waiters_head != NULL);
    ccol_mutex_unlock(cq->mutex);
    if (has_sel_waiters) {
      ccol_assert(false);
    }

    /* An empty waiter list alone does not mean that nothing uses this queue.
     * The reactor can collect a callback before ccol_event_loop_remove runs.
     * That callback holds the queue through its own snapshot, and it is
     * about to call into the queue. See _queue_wait_for_dispatch_refs. */
    _queue_wait_for_dispatch_refs(&cq->dispatch_refs, cq);

    /* The count is read only once no callback can touch the queue any more.
     * A write callback that was already running when its registration was
     * removed can still send; a count read before the wait misses that
     * message, and its data pointer is then freed with the queue without a
     * trace. */
    if (ccol_circq_msg_count(cq) > 0) {
      ccol_assert(false);
    }

    if (cq->msg_array) {
      _ccol_mem_free(cq->m_procs, cq->msg_array);
      cq->msg_array = NULL;
    }

#if CCOL_FORK_SAFETY_REQUIRED
    /* The code unregisters the mutex before it destroys the mutex. After the
     * destroy, &cq->mutex must never again be a candidate that
     * _cthreadcomm_atfork_prepare can lock. */
    _queue_mutex_registry_remove(&cq->mutex, &cq->registry_idx);
#endif

    ccol_mutex_destroy(cq->mutex);
    ccol_cond_var_destroy(cq->read_cond);
    ccol_cond_var_destroy(cq->write_cond);

    if (cq->m_procs) {
      ccol_free_t free_func = cq->m_procs->free;
      free_func(cq->m_procs);
      free_func(cq);
    } else {
      ccol_mem_free(cq);
    }
  }
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. These two functions lock and unlock the internal mutex of cq
 * directly. They go around every public API function. A test can therefore
 * hold the mutex locked for a window of any length that it chooses. See the
 * doc comments of these functions in cthreadcomm.h. */
void ccol_circq_test_lock_mutex_for_tests(ccol_circular_queue *cq) {
  ccol_mutex_lock(cq->mutex);
}

void ccol_circq_test_unlock_mutex_for_tests(ccol_circular_queue *cq) {
  ccol_mutex_unlock(cq->mutex);
}

/* Test-only. Reports whether a ccol_select() caller is linked into the
 * read-waiter list of cq right now. The function does one self-contained
 * lock, check and unlock. It is not built from the two functions above. A
 * test that composed those two would hold the mutex across its own check
 * with no bound. That is the exact deadlock that this helper lets a test
 * avoid.
 *
 * A test can need a select-waiter thread to REALLY reach the
 * ccol_mutex_lock and link step of Phase 1 before the test goes on. One
 * example is a test that destroys cq, to exercise the has_sel_waiters misuse
 * check in __ccol_circular_queue_destroy every time. Such a test polls this
 * function in a bounded loop. The other design is a guess. It hopes that a
 * fixed sleep gives the OS enough margin to schedule a brand-new thread all
 * the way to that point. A return from pthread_create() gives no such
 * guarantee. A sleep-based guess that loses this race lets destroy() go on
 * as if nobody watched the queue. It then frees cq under the late waiter, at
 * the moment that the waiter finally runs. The thread-start scheduling
 * latency of qemu-user emulation is much higher and much more variable, and
 * a fixed 50ms margin is not reliably enough there. */
bool ccol_circq_test_has_sel_read_waiter_for_tests(ccol_circular_queue *cq) {
  ccol_mutex_lock(cq->mutex);
  bool has_waiter = cq->sel_read_waiters_head != NULL;
  ccol_mutex_unlock(cq->mutex);
  return has_waiter;
}

/* Test-only. Gives how many waiter nodes are linked into the read-waiter
 * list of cq right now. A test can need a SPECIFIC number of ccol_select()
 * callers to reach their own link step, and not only "at least one". Such a
 * test polls this function. See the doc comment of this function in
 * cthreadcomm.h. */
size_t ccol_circq_test_sel_read_waiter_count_for_tests(
    ccol_circular_queue *cq) {
  ccol_mutex_lock(cq->mutex);
  size_t n = 0;
  for (ccol_sel_waiter *w = cq->sel_read_waiters_head; w; w = w->next) ++n;
  ccol_mutex_unlock(cq->mutex);
  return n;
}

/* Test-only. These are the counters that gate the condition variable signals
 * of the send and receive paths. A test polls one of them to learn that a
 * background thread really parked in the matching wait. The test then does
 * the operation that must wake that thread. This is what makes such a test
 * deterministic and non-vacuous: a wait site that does not register itself
 * never lets the poll succeed. The read happens under the mutex, which a
 * parked waiter already unlocked. */
size_t ccol_circq_waiting_readers_for_tests(ccol_circular_queue *cq) {
  ccol_mutex_lock(cq->mutex);
  size_t n = cq->readers_waiting;
  ccol_mutex_unlock(cq->mutex);
  return n;
}

size_t ccol_circq_waiting_writers_for_tests(ccol_circular_queue *cq) {
  ccol_mutex_lock(cq->mutex);
  size_t n = cq->writers_waiting;
  ccol_mutex_unlock(cq->mutex);
  return n;
}
#endif

/* Writes msg into the circular array at write_index. It then advances the
 * index, and the index wraps to 0 at max_size. The function sets msg->data to
 * NULL, which transfers ownership to the receiver. That is the zero-copy
 * contract. The caller must hold the mutex. */
static void _sendto_cq(ccol_circular_queue *cq, c_message_t *msg) {
  cq->msg_array[cq->write_index].data = msg->data;
  cq->msg_array[cq->write_index++].size = (msg->data == NULL) ? 0 : msg->size;
  msg->data = NULL;
  if (cq->write_index == cq->max_size) {
    cq->write_index = 0;
  }
  ++cq->msg_count;

  if (cq->readers_waiting) ccol_cond_var_signal(cq->read_cond);
  notify_one_sel_waiter(&cq->sel_read_waiters_head, &cq->sel_read_rotor);
}

/* Validates the arguments of a send. The queue and the message must not be
 * NULL. The data and size fields of a message must agree with each other.
 * When data is not NULL, size must be larger than 0, because a live pointer
 * must not carry an empty payload. When data is NULL, size must be 0, because
 * a NULL pointer with a non-zero size is an inconsistent value. */
static bool verify_circq_send_zc_params(ccol_circular_queue *cq,
                                        c_message_t *msg) {
  if (!cq || !msg || (msg->size == 0 && msg->data != NULL) ||
      (msg->data == NULL && msg->size != 0)) {
    return false;
  }

  return true;
}

/* A send that blocks. It waits on write_cond until the queue has space. It
 * then transfers the ownership of msg->data to the queue. It returns
 * ccol_not_permitted immediately when the write direction is turned off. The
 * function checks that state before the wait and after the wait, to handle a
 * race with ccol_circq_disable_sending. */
ccol_retval_t ccol_circq_send_zc(ccol_circular_queue *cq, c_message_t *msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  ccol_mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  while (cq->msg_count == cq->max_size && !cq->writing_disabled) {
    ++cq->writers_waiting;
    ccol_cond_var_wait(cq->write_cond, cq->mutex);
    --cq->writers_waiting;
  }

  if (cq->writing_disabled) {
    ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  _sendto_cq(cq, msg);

  ccol_mutex_unlock(cq->mutex);

  return ccol_success;
}

/* A send that does not block. It returns ccol_container_full immediately when
 * the queue is full, and it does not wait. It returns ccol_not_permitted when
 * the write direction is turned off.
 */
ccol_retval_t ccol_circq_try_send_zc(ccol_circular_queue *cq,
                                     c_message_t *msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_full;

  ccol_mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  if (cq->msg_count < cq->max_size) {
    _sendto_cq(cq, msg);
    result = ccol_success;
  }

  ccol_mutex_unlock(cq->mutex);

  return result;
}

/* A forward declaration. The definition is below, directly before
 * ccol_circq_recv_zc. The declaration is needed here for the racing-consumer
 * simulation of ccol_circq_timed_send_zc, which only exists under
 * RUNNING_UNIT_TESTS. */
static void _recvfrom_cq(ccol_circular_queue *cq, c_message_t *target_buf);

#ifdef RUNNING_UNIT_TESTS
/* Test-only hooks. They force the next ccol_cond_var_timedwait call inside
 * the wait loop of ccol_circq_timed_send_zc to report EINVAL instead of a
 * real wait outcome. The hook then disarms itself. The _racing_ready variant
 * also frees the queue accounting of one slot. It does this with
 * _recvfrom_cq, which is exactly the work of a real concurrent consumer that
 * completes its receive. It runs under the same cq->mutex that this call
 * already holds. This simulates a consumer whose own wakeup completed a
 * moment before the code sees the unrelated, forced error. The reasoning of
 * ccol_select_test_force_next_condvar_wait_error_racing_ready is the same.
 * The code skips the real wait call. It also does not unlock the mutex. A
 * truly concurrent thread can therefore never race in here on its own. This
 * is the only way for a test to reach that interleave every time. The
 * message data of the freed slot goes back to the test through
 * *racing_ready_msg_out, when there is such data. A test can then verify
 * it. Without that, the code here would discard the data and report
 * nothing. */
static _Atomic bool g_circq_send_force_condvar_wait_error = false;
static _Atomic bool g_circq_send_force_condvar_wait_error_also_race = false;
static c_message_t g_circq_send_race_freed_msg = {0};

void ccol_circq_test_force_next_send_condvar_wait_error(void) {
  atomic_store(&g_circq_send_force_condvar_wait_error, true);
}

void ccol_circq_test_force_next_send_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_circq_send_force_condvar_wait_error, true);
  atomic_store(&g_circq_send_force_condvar_wait_error_also_race, true);
}

c_message_t ccol_circq_test_take_race_freed_msg(void) {
  c_message_t m = g_circq_send_race_freed_msg;
  g_circq_send_race_freed_msg = (c_message_t){0};
  return m;
}
#endif

/* A send with a timeout. It waits for space for at most timeout_us
 * microseconds. A timeout_us of 0 is the try variant, exactly. The code
 * computes the absolute deadline once, before the wait loop. This is why
 * repeated spurious wakes cannot extend the timeout. The function returns
 * ccol_timed_out when the deadline passes. */
ccol_retval_t ccol_circq_timed_send_zc(ccol_circular_queue *cq,
                                       c_message_t *msg, uint64_t timeout_us) {
  if (timeout_us == 0) return ccol_circq_try_send_zc(cq, msg);
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  ccol_mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  if (cq->msg_count == cq->max_size) {
    int retval;
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);

    while (cq->msg_count == cq->max_size && !cq->writing_disabled) {
      /* The code counts this wait too, not only the untimed wait. A sender
       * signals only when this count is not zero. Without the count here, a
       * waiter does not register itself. It then sleeps until its own
       * timeout while a message is already queued for it. */
      ++cq->writers_waiting;
#ifdef RUNNING_UNIT_TESTS
      if (atomic_load(&g_circq_send_force_condvar_wait_error)) {
        atomic_store(&g_circq_send_force_condvar_wait_error, false);
        retval = EINVAL;
        if (atomic_load(&g_circq_send_force_condvar_wait_error_also_race)) {
          atomic_store(&g_circq_send_force_condvar_wait_error_also_race, false);
          _recvfrom_cq(cq, &g_circq_send_race_freed_msg);
        }
      } else {
        retval = ccol_cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
      }
#else
      retval = ccol_cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
#endif
      --cq->writers_waiting;
      if (retval) {
        /* The code checks the state again under the mutex before it commits
         * to either outcome below. It does this whatever the return value of
         * ccol_cond_var_timedwait suggests. ccol_cond_var_timedwait always
         * takes cq->mutex again before it returns, on success and on
         * failure. The _recvfrom_cq of a concurrent consumer needs cq->mutex
         * to free a slot and to signal write_cond. That consumer can
         * therefore finish and hand the mutex back to this exact call. It
         * can do this a moment before the code also sees an unrelated,
         * spurious error that is not ETIMEDOUT. Without this second check,
         * the code discards that real, available slot. It then reports
         * ccol_unexpected_failure and does not use the slot for the send.
         * The FAILURE branch of _sel_wait_condvar uses the same reasoning.
         * This check also covers the ordinary ETIMEDOUT case. There, a
         * consumer can free a slot, or writing_disabled can change. Such a
         * change can come after the kernel detects the expiry and before
         * this thread takes the mutex again. */
        if (cq->msg_count < cq->max_size || cq->writing_disabled) break;
        if (retval != ETIMEDOUT) {
          _ccol_store_wait_errno(retval);
          ccol_mutex_unlock(cq->mutex);
          return ccol_unexpected_failure;
        }
        ccol_mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
  }

  if (cq->writing_disabled) {
    ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  _sendto_cq(cq, msg);

  ccol_mutex_unlock(cq->mutex);

  return ccol_success;
}

/* Reads one message from the circular array at read_index. It then advances
 * the index, and the index wraps to 0 at max_size. It signals write_cond when
 * a sender blocks on that condition variable. The caller must hold the
 * mutex. */
static void _recvfrom_cq(ccol_circular_queue *cq, c_message_t *target_buf) {
  target_buf->data = cq->msg_array[cq->read_index].data;
  target_buf->size = cq->msg_array[cq->read_index++].size;
  if (cq->read_index == cq->max_size) {
    cq->read_index = 0;
  }

  --cq->msg_count;

  if (cq->writers_waiting) ccol_cond_var_signal(cq->write_cond);
  notify_one_sel_waiter(&cq->sel_write_waiters_head, &cq->sel_write_rotor);
}

/* Validates the arguments of a receive. The queue and the target buffer must
 * not be NULL. */
static bool verify_recvfrom_cq_zc_params(ccol_circular_queue *cq,
                                         c_message_t *target_buf) {
  if (!cq || !target_buf) {
    return false;
  }

  return true;
}

/* A receive that blocks. It waits on read_cond until the queue holds at least
 * one message. It then transfers the ownership to target_buf. */
ccol_retval_t ccol_circq_recv_zc(ccol_circular_queue *cq,
                                 c_message_t *target_buf) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_mutex_lock(cq->mutex);

  while (cq->msg_count == 0) {
    ++cq->readers_waiting;
    ccol_cond_var_wait(cq->read_cond, cq->mutex);
    --cq->readers_waiting;
  }

  _recvfrom_cq(cq, target_buf);

  ccol_mutex_unlock(cq->mutex);

  return ccol_success;
}

/* A receive that does not block. It returns ccol_container_empty immediately
 * when the queue holds no message. */
ccol_retval_t ccol_circq_try_recv_zc(ccol_circular_queue *cq,
                                     c_message_t *target_buf) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_empty;

  ccol_mutex_lock(cq->mutex);

  if (cq->msg_count > 0) {
    result = ccol_success;
    _recvfrom_cq(cq, target_buf);
  }

  ccol_mutex_unlock(cq->mutex);

  return result;
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only hooks. They force the next ccol_cond_var_timedwait call inside
 * the wait loop of ccol_circq_timed_recv_zc to report EINVAL instead of a
 * real wait outcome. The hook then disarms itself. The _racing_ready variant
 * also puts a sentinel message into the queue. It does this with _sendto_cq,
 * which is exactly the work of a real concurrent producer that completes its
 * send. It runs under the same cq->mutex that this call already holds. This
 * simulates a producer whose own wakeup completed a moment before the code
 * sees the unrelated, forced error. The reasoning of
 * ccol_select_test_force_next_condvar_wait_error_racing_ready is the same.
 * The code skips the real wait call. It also does not unlock the mutex. A
 * truly concurrent thread can therefore never race in here on its own. */
static _Atomic bool g_circq_recv_force_condvar_wait_error = false;
static _Atomic bool g_circq_recv_force_condvar_wait_error_also_race = false;

void ccol_circq_test_force_next_recv_condvar_wait_error(void) {
  atomic_store(&g_circq_recv_force_condvar_wait_error, true);
}

void ccol_circq_test_force_next_recv_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_circq_recv_force_condvar_wait_error, true);
  atomic_store(&g_circq_recv_force_condvar_wait_error_also_race, true);
}
#endif

/* A receive with a timeout. It waits for a message for at most timeout_us
 * microseconds. A timeout_us of 0 is the try variant, exactly. It uses the
 * same absolute deadline strategy as ccol_circq_timed_send_zc. That strategy
 * prevents timeout drift on a spurious wake.
 */
ccol_retval_t ccol_circq_timed_recv_zc(ccol_circular_queue *cq,
                                       c_message_t *target_buf,
                                       uint64_t timeout_us) {
  if (timeout_us == 0) return ccol_circq_try_recv_zc(cq, target_buf);
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_mutex_lock(cq->mutex);

  if (cq->msg_count == 0) {
    int retval;
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);

    while (cq->msg_count == 0) {
      /* The code counts this wait too, not only the untimed wait. A sender
       * signals only when this count is not zero. Without the count here, a
       * waiter does not register itself. It then sleeps until its own
       * timeout while a message is already queued for it. */
      ++cq->readers_waiting;
#ifdef RUNNING_UNIT_TESTS
      if (atomic_load(&g_circq_recv_force_condvar_wait_error)) {
        atomic_store(&g_circq_recv_force_condvar_wait_error, false);
        retval = EINVAL;
        if (atomic_load(&g_circq_recv_force_condvar_wait_error_also_race)) {
          atomic_store(&g_circq_recv_force_condvar_wait_error_also_race, false);
          c_message_t sentinel = {.data = NULL, .size = 0};
          _sendto_cq(cq, &sentinel);
        }
      } else {
        retval = ccol_cond_var_timedwait(cq->read_cond, cq->mutex, abs_time);
      }
#else
      retval = ccol_cond_var_timedwait(cq->read_cond, cq->mutex, abs_time);
#endif
      --cq->readers_waiting;
      if (retval) {
        /* The code checks the state again under the mutex before it commits
         * to either outcome below. It does this whatever the return value of
         * ccol_cond_var_timedwait suggests. ccol_cond_var_timedwait always
         * takes cq->mutex again before it returns, on success and on
         * failure. The _sendto_cq of a concurrent producer needs cq->mutex
         * to add a message and to signal read_cond. That producer can
         * therefore finish and hand the mutex back to this exact call. It
         * can do this a moment before the code also sees an unrelated,
         * spurious error that is not ETIMEDOUT. Without this second check,
         * the code discards that real, arrived message. It then reports
         * ccol_unexpected_failure and does not receive the message. The
         * FAILURE branch of _sel_wait_condvar uses the same reasoning. This
         * check also covers the ordinary ETIMEDOUT case. There, a producer
         * can add a message after the kernel detects the expiry and before
         * this thread takes the mutex again. */
        if (cq->msg_count > 0) break;
        if (retval != ETIMEDOUT) {
          _ccol_store_wait_errno(retval);
          ccol_mutex_unlock(cq->mutex);
          return ccol_unexpected_failure;
        }
        ccol_mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
  }

  _recvfrom_cq(cq, target_buf);

  ccol_mutex_unlock(cq->mutex);

  return ccol_success;
}

/* Sets the writing_disabled flag. It then broadcasts on write_cond to wake
 * every thread that blocks in ccol_circq_send_zc. Each of those threads then
 * sees the new state and returns ccol_not_permitted. */
ccol_retval_t ccol_circq_disable_sending(ccol_circular_queue *cq) {
  if (cq) {
    ccol_mutex_lock(cq->mutex);
    cq->writing_disabled = true;
    ccol_cond_var_broadcast(cq->write_cond);
    notify_all_sel_waiters(cq->sel_write_waiters_head);
    ccol_mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

/* Clears the writing_disabled flag. It then broadcasts on write_cond to wake
 * every thread that blocked while the write direction was turned off. */
ccol_retval_t ccol_circq_enable_sending(ccol_circular_queue *cq) {
  if (cq) {
    ccol_mutex_lock(cq->mutex);
    cq->writing_disabled = false;
    ccol_cond_var_broadcast(cq->write_cond);
    notify_all_sel_waiters(cq->sel_write_waiters_head);
    ccol_mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

/* Returns how many messages the queue holds now. It takes the mutex to get a
 * consistent snapshot. It returns ccol_invalid_size when cq is NULL. */
size_t ccol_circq_msg_count(ccol_circular_queue *cq) {
  size_t result = ccol_invalid_size;

  if (cq) {
    ccol_mutex_lock(cq->mutex);
    result = cq->msg_count;
    ccol_mutex_unlock(cq->mutex);
  }

  return result;
}

/* The section about the dynamic queue starts here. */
typedef struct dllist_node {
  struct dllist_node *prev;
  c_message_t msg;
  struct dllist_node *next;
} dllist_node;

struct ccol_dynamic_queue {
  ccol_mutex_t mutex;
  ccol_cond_var_t read_cond;

  /* See the same field on ccol_circular_queue for the reason why a gate
   * controls a signal here, instead of a signal on every operation. This
   * queue has no bound, so it has readers to wake but never writers to
   * block. */
  size_t readers_waiting;

  size_t msg_count;

  dllist_node *head;
  dllist_node *tail;

  ccol_memmgmt_procs_t *m_procs;

  bool writing_disabled;

  ccol_sel_waiter *sel_read_waiters_head;
  ccol_sel_waiter *sel_write_waiters_head;

  /* Round-robin cursors for notify_one_sel_waiter. See the doc comment of
   * that function. NULL means "start a fresh cycle at the matching head". */
  ccol_sel_waiter *sel_read_rotor;
  ccol_sel_waiter *sel_write_rotor;

  /* See the same field on ccol_circular_queue. */
  _Atomic size_t dispatch_refs;

#if CCOL_FORK_SAFETY_REQUIRED
  /* See the same field on ccol_circular_queue. */
  size_t registry_idx;
#endif
};

/* Allocates a new dllist_node and copies the metadata of the message into it.
 * It sets msg->data to NULL, which transfers the ownership. It then appends
 * the node to the tail of the doubly-linked list of the queue. The caller
 * must hold the mutex. */
static ccol_retval_t append_msg_to_dq_tail(ccol_dynamic_queue *dq,
                                           c_message_t *msg) {
  dllist_node *new_elem =
      (dllist_node *)_ccol_mem_alloc(dq->m_procs, sizeof(dllist_node));
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

/* Removes the message of the head node and gives it to the caller. When the
 * list becomes empty, the function sets head and tail to NULL, which keeps
 * the invariant consistent. It frees the node struct after it copies the
 * message out. The caller must hold the mutex. */
static ccol_retval_t remove_msg_from_dq_head(ccol_dynamic_queue *dq,
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

  _ccol_mem_free(dq->m_procs, node_to_be_freed);
  return ccol_success;
}

/* Frees every dllist_node struct in the dynamic queue. It does not free the
 * data pointer in each message. A consumer must already have taken those
 * pointers. The destroy function asserts on a msg_count that is not zero, to
 * catch such a leak. */
static void destroy_dq_dllist(ccol_dynamic_queue *dq) {
  dllist_node *node_to_be_freed = NULL;
  while (dq->head) {
    node_to_be_freed = dq->head;
    dq->head = dq->head->next;
    _ccol_mem_free(dq->m_procs, node_to_be_freed);
  }
  dq->tail = NULL;
}

/* Allocates and initialises a dynamic queue with no bound. A doubly-linked
 * list backs the queue. A send never blocks, because the list grows with each
 * message. A send on a ccol_circular_queue does block. This queue needs only
 * a read_cond. It needs no write_cond. */
ccol_dynamic_queue *ccol_dynamic_queue_create_with_mprocs(
    ccol_memmgmt_procs_t *mmgmt_procs, char **err_str) {
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  ccol_dynamic_queue *dq = (ccol_dynamic_queue *)_ccol_mem_alloc(
      mmgmt_procs, sizeof(ccol_dynamic_queue));
  if (!dq) {
    if (err_str) {
      *err_str =
          CCOL_ERR_STR("Failed to allocate memory for ccol_dynamic_queue");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(dq, mmgmt_procs, err_str)) {
    _ccol_mem_free(mmgmt_procs, dq);
    return NULL;
  }

  if (ccol_mutex_init(dq->mutex) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to initialize dq mutex");
    _ccol_mem_free(mmgmt_procs, dq->m_procs);
    _ccol_mem_free(mmgmt_procs, dq);
    return NULL;
  }
  if (_init_monotonic_cond_var(&dq->read_cond) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to initialize dq read_cond");
    ccol_mutex_destroy(dq->mutex);
    _ccol_mem_free(mmgmt_procs, dq->m_procs);
    _ccol_mem_free(mmgmt_procs, dq);
    return NULL;
  }
  dq->msg_count = 0;
  dq->head = NULL;
  dq->tail = NULL;
  /* The code sets this explicitly, for the same reason as the counters of
   * ccol_circular_queue. See the note there. */
  dq->readers_waiting = 0;
  dq->writing_disabled = false;
  dq->sel_read_waiters_head = NULL;
  dq->sel_write_waiters_head = NULL;
  dq->sel_read_rotor = NULL;
  dq->sel_write_rotor = NULL;
  atomic_init(&dq->dispatch_refs, (size_t)0);

#if CCOL_FORK_SAFETY_REQUIRED
  /* This registration is the LAST step, after dq is fully built. See the same
   * call in ccol_circular_queue_create_with_mprocs for the full reason, which
   * holds here too. A ccol_dynamic_queue has no msg_array of its own to
   * unwind, because it starts as an empty linked list. */
  if (!_queue_mutex_registry_add(&dq->mutex, &dq->registry_idx)) {
    if (err_str) {
      *err_str = CCOL_ERR_STR(
          "Failed to register ccol_dynamic_queue's mutex for fork safety");
    }
    ccol_mutex_destroy(dq->mutex);
    ccol_cond_var_destroy(dq->read_cond);
    _ccol_mem_free(mmgmt_procs, dq->m_procs);
    _ccol_mem_free(mmgmt_procs, dq);
    return NULL;
  }
#endif

  if (err_str) {
    *err_str = NULL;
  }

  return dq;
}

/* Destroys the dynamic queue. It asserts when a message remains, like
 * __ccol_circular_queue_destroy. That assert makes a data pointer that nobody
 * cleaned up visible as a bug. It also asserts when a waiter of ccol_select()
 * or of ccol_event_loop is still linked into one of the waiter lists. See the
 * comment of __ccol_circular_queue_destroy for the reason why the second case
 * is a real use-after-free hazard and not only a leak. The still-linked node
 * then holds a dangling &dq->mutex. */
void __ccol_dynamic_queue_destroy(ccol_dynamic_queue *dq) {
  if (dq) {
    /* This read happens under the mutex. See the same check in
     * __ccol_circular_queue_destroy for the reason why the read must be
     * race-free and not an unlocked peek. */
    ccol_mutex_lock(dq->mutex);
    bool has_sel_waiters = (dq->sel_read_waiters_head != NULL) ||
                           (dq->sel_write_waiters_head != NULL);
    ccol_mutex_unlock(dq->mutex);
    if (has_sel_waiters) {
      ccol_assert(false);
    }

    /* See the same wait, and the count check after it, in
     * __ccol_circular_queue_destroy. */
    _queue_wait_for_dispatch_refs(&dq->dispatch_refs, dq);

    if (ccol_dynmq_msg_count(dq) > 0) {
      ccol_assert(false);
    }

#if CCOL_FORK_SAFETY_REQUIRED
    /* The code unregisters the mutex before it destroys the mutex. See the
     * same call in __ccol_circular_queue_destroy for the reason. */
    _queue_mutex_registry_remove(&dq->mutex, &dq->registry_idx);
#endif

    ccol_mutex_destroy(dq->mutex);
    ccol_cond_var_destroy(dq->read_cond);
    destroy_dq_dllist(dq);

    if (dq->m_procs) {
      ccol_free_t free_func = dq->m_procs->free;
      free_func(dq->m_procs);
      free_func(dq);
    } else {
      ccol_mem_free(dq);
    }
  }
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. This is the counter that gates the read_cond signal of the send
 * path of the dynamic queue. See ccol_circq_waiting_readers_for_tests for the
 * reason why a test polls this counter. A test must not guess that a
 * background thread already parked. */
size_t ccol_dynmq_waiting_readers_for_tests(ccol_dynamic_queue *dq) {
  ccol_mutex_lock(dq->mutex);
  size_t n = dq->readers_waiting;
  ccol_mutex_unlock(dq->mutex);
  return n;
}
#endif

/* Appends msg to the tail of the dynamic queue. It signals read_cond when a
 * receiver blocks on that condition variable. The caller must hold the mutex.
 * It returns ccol_not_enough_memory when the allocation fails, and it then
 * does not change msg->data. */
static ccol_retval_t _sendto_dq(ccol_dynamic_queue *dq, c_message_t *msg) {
  ccol_retval_t retval = append_msg_to_dq_tail(dq, msg);

  if (retval == ccol_success) {
    ++dq->msg_count;
    if (dq->readers_waiting) ccol_cond_var_signal(dq->read_cond);
    notify_one_sel_waiter(&dq->sel_read_waiters_head, &dq->sel_read_rotor);
  }

  return retval;
}

/* Validates the arguments of a send on a dynamic queue. The checks are the
 * same as in verify_circq_send_zc_params, but for a ccol_dynamic_queue. */
static bool verify_dynmq_send_zc_params(ccol_dynamic_queue *dq,
                                        c_message_t *msg) {
  if (!dq || !msg || (msg->size == 0 && msg->data != NULL) ||
      (msg->data == NULL && msg->size != 0)) {
    return false;
  }

  return true;
}

/* A send to the dynamic queue that does not block. The queue has no bound, so
 * it never waits for space. It returns ccol_not_permitted when the write
 * direction is turned off. It returns ccol_container_full when msg_count
 * reaches ccol_max_elem_count. */
ccol_retval_t ccol_dynmq_send_zc(ccol_dynamic_queue *dq, c_message_t *msg) {
  if (!verify_dynmq_send_zc_params(dq, msg)) {
    return ccol_invalid_args;
  }

  ccol_mutex_lock(dq->mutex);

  if (dq->writing_disabled) {
    ccol_mutex_unlock(dq->mutex);
    return ccol_not_permitted;
  }

  if (dq->msg_count == ccol_max_elem_count) {
    ccol_mutex_unlock(dq->mutex);
    return ccol_container_full;
  }

  ccol_retval_t result = _sendto_dq(dq, msg);

  ccol_mutex_unlock(dq->mutex);

  return result;
}

/* Removes the head message from the dynamic queue and decrements msg_count.
 * The caller must hold the mutex. */
static ccol_retval_t _recvfrom_dq(ccol_dynamic_queue *dq,
                                  c_message_t *target_buf) {
  ccol_retval_t retval = remove_msg_from_dq_head(dq, target_buf);

  if (retval == ccol_success) {
    --dq->msg_count;
    notify_one_sel_waiter(&dq->sel_write_waiters_head, &dq->sel_write_rotor);
  }

  return retval;
}

/* Validates the arguments of a receive on a dynamic queue. */
static bool verify_recvfrom_dq_zc_params(ccol_dynamic_queue *dq,
                                         c_message_t *target_buf) {
  if (!dq || !target_buf) {
    return false;
  }

  return true;
}

/* A receive from the dynamic queue that blocks. It waits on read_cond until
 * the queue holds at least one message. It then pops that message from the
 * head. */
ccol_retval_t ccol_dynmq_recv_zc(ccol_dynamic_queue *dq,
                                 c_message_t *target_buf) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_mutex_lock(dq->mutex);

  while (dq->msg_count == 0) {
    ++dq->readers_waiting;
    ccol_cond_var_wait(dq->read_cond, dq->mutex);
    --dq->readers_waiting;
  }

  ccol_retval_t result = _recvfrom_dq(dq, target_buf);

  ccol_mutex_unlock(dq->mutex);

  return result;
}

/* A receive that does not block. It returns ccol_container_empty immediately
 * when the dynamic queue is empty. */
ccol_retval_t ccol_dynmq_try_recv_zc(ccol_dynamic_queue *dq,
                                     c_message_t *target_buf) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_empty;

  ccol_mutex_lock(dq->mutex);

  if (dq->msg_count > 0) {
    result = _recvfrom_dq(dq, target_buf);
  }

  ccol_mutex_unlock(dq->mutex);

  return result;
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only hooks. They force the next ccol_cond_var_timedwait call inside
 * the wait loop of ccol_dynmq_timed_recv_zc to report EINVAL instead of a
 * real wait outcome. The hook then disarms itself. The _racing_ready variant
 * also puts a sentinel message into the queue. It does this with _sendto_dq,
 * which is exactly the work of a real concurrent producer that completes its
 * send. It runs under the same dq->mutex that this call already holds. This
 * simulates a producer whose own wakeup completed a moment before the code
 * sees the unrelated, forced error. The reasoning of
 * ccol_select_test_force_next_condvar_wait_error_racing_ready is the same.
 * The code skips the real wait call. It also does not unlock the mutex. A
 * truly concurrent thread can therefore never race in here on its own. */
static _Atomic bool g_dynmq_recv_force_condvar_wait_error = false;
static _Atomic bool g_dynmq_recv_force_condvar_wait_error_also_race = false;

void ccol_dynmq_test_force_next_recv_condvar_wait_error(void) {
  atomic_store(&g_dynmq_recv_force_condvar_wait_error, true);
}

void ccol_dynmq_test_force_next_recv_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_dynmq_recv_force_condvar_wait_error, true);
  atomic_store(&g_dynmq_recv_force_condvar_wait_error_also_race, true);
}
#endif

/* A receive from the dynamic queue with a timeout. A timeout_us of 0 is the
 * try variant, exactly. It uses the same absolute deadline approach as the
 * timed variants of the circular queue. */
ccol_retval_t ccol_dynmq_timed_recv_zc(ccol_dynamic_queue *dq,
                                       c_message_t *target_buf,
                                       uint64_t timeout_us) {
  if (timeout_us == 0) return ccol_dynmq_try_recv_zc(dq, target_buf);
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_mutex_lock(dq->mutex);

  if (dq->msg_count == 0) {
    int retval;
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);

    while (dq->msg_count == 0) {
      /* The code counts this wait too, not only the untimed wait. A sender
       * signals only when this count is not zero. Without the count here, a
       * waiter does not register itself. It then sleeps until its own
       * timeout while a message is already queued for it. */
      ++dq->readers_waiting;
#ifdef RUNNING_UNIT_TESTS
      if (atomic_load(&g_dynmq_recv_force_condvar_wait_error)) {
        atomic_store(&g_dynmq_recv_force_condvar_wait_error, false);
        retval = EINVAL;
        if (atomic_load(&g_dynmq_recv_force_condvar_wait_error_also_race)) {
          atomic_store(&g_dynmq_recv_force_condvar_wait_error_also_race, false);
          c_message_t sentinel = {.data = NULL, .size = 0};
          _sendto_dq(dq, &sentinel);
        }
      } else {
        retval = ccol_cond_var_timedwait(dq->read_cond, dq->mutex, abs_time);
      }
#else
      retval = ccol_cond_var_timedwait(dq->read_cond, dq->mutex, abs_time);
#endif
      --dq->readers_waiting;
      if (retval) {
        /* The code checks the state again under the mutex before it commits
         * to either outcome below. It does this whatever the return value of
         * ccol_cond_var_timedwait suggests. ccol_cond_var_timedwait always
         * takes dq->mutex again before it returns, on success and on
         * failure. The _sendto_dq of a concurrent producer needs dq->mutex
         * to add a message and to signal read_cond. That producer can
         * therefore finish and hand the mutex back to this exact call. It
         * can do this a moment before the code also sees an unrelated,
         * spurious error that is not ETIMEDOUT. Without this second check,
         * the code discards that real, arrived message. It then reports
         * ccol_unexpected_failure and does not receive the message. The
         * FAILURE branch of _sel_wait_condvar uses the same reasoning. This
         * check also covers the ordinary ETIMEDOUT case. There, a producer
         * can add a message after the kernel detects the expiry and before
         * this thread takes the mutex again. */
        if (dq->msg_count > 0) break;
        if (retval != ETIMEDOUT) {
          _ccol_store_wait_errno(retval);
          ccol_mutex_unlock(dq->mutex);
          return ccol_unexpected_failure;
        }
        ccol_mutex_unlock(dq->mutex);
        return ccol_timed_out;
      }
    }
  }

  ccol_retval_t result = _recvfrom_dq(dq, target_buf);

  ccol_mutex_unlock(dq->mutex);

  return result;
}

/* Sets the writing_disabled flag on the dynamic queue. */
ccol_retval_t ccol_dynmq_disable_sending(ccol_dynamic_queue *dq) {
  if (dq) {
    ccol_mutex_lock(dq->mutex);
    dq->writing_disabled = true;
    notify_all_sel_waiters(dq->sel_write_waiters_head);
    ccol_mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

/* Clears the writing_disabled flag on the dynamic queue. */
ccol_retval_t ccol_dynmq_enable_sending(ccol_dynamic_queue *dq) {
  if (dq) {
    ccol_mutex_lock(dq->mutex);
    dq->writing_disabled = false;
    notify_all_sel_waiters(dq->sel_write_waiters_head);
    ccol_mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

/* Returns how many messages the dynamic queue holds. It returns
 * ccol_invalid_size when dq is NULL. */
size_t ccol_dynmq_msg_count(ccol_dynamic_queue *dq) {
  size_t result = ccol_invalid_size;

  if (dq) {
    ccol_mutex_lock(dq->mutex);
    result = dq->msg_count;
    ccol_mutex_unlock(dq->mutex);
  }

  return result;
}

/* The section about the channel starts here. */
struct ccol_channel {
  ccol_thread_id_t owner_tid;
  ccol_circular_queue *owner_to_workers_cq;
  ccol_circular_queue *workers_to_owner_cq;
  ccol_memmgmt_procs_t *m_procs;
};

/* Creates a ccol_channel that carries messages in two directions. It uses two
 * circular queues. One queue goes from the owner to the workers. The other
 * queue goes from the workers back to the owner. The ccol_channel records the
 * ID of the thread that creates it as the owner_tid. ccol_chan_send_zc and
 * ccol_chan_recv_zc then route to the correct queue automatically. */
ccol_channel *ccol_channel_create_with_mprocs(size_t max_size,
                                              ccol_memmgmt_procs_t *mmgmt_procs,
                                              char **err_str) {
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  ccol_channel *ch =
      (ccol_channel *)_ccol_mem_alloc(mmgmt_procs, sizeof(ccol_channel));
  if (!ch) {
    if (err_str) {
      *err_str = CCOL_ERR_STR("Failed to allocate memory for ccol_channel");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(ch, mmgmt_procs, err_str)) {
    _ccol_mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->owner_to_workers_cq =
      ccol_circular_queue_create_with_mprocs(max_size, mmgmt_procs, err_str);
  if (!ch->owner_to_workers_cq) {
    _ccol_mem_free(mmgmt_procs, ch->m_procs);
    _ccol_mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->workers_to_owner_cq =
      ccol_circular_queue_create_with_mprocs(max_size, mmgmt_procs, err_str);
  if (!ch->workers_to_owner_cq) {
    ccol_circular_queue_destroy(ch->owner_to_workers_cq);
    _ccol_mem_free(mmgmt_procs, ch->m_procs);
    _ccol_mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->owner_tid = ccol_get_thread_id();

  if (err_str) {
    *err_str = NULL;
  }

  return ch;
}

/* Destroys both circular queues below the channel. It then frees the
 * ccol_channel struct.
 */
void __ccol_channel_destroy(ccol_channel *ch) {
  if (ch) {
    ccol_circular_queue_destroy(ch->owner_to_workers_cq);
    ccol_circular_queue_destroy(ch->workers_to_owner_cq);

    if (ch->m_procs) {
      ccol_free_t free_func = ch->m_procs->free;
      free_func(ch->m_procs);
      free_func(ch);
    } else {
      ccol_mem_free(ch);
    }
  }
}

/* A send on the ccol_channel that blocks. It routes to owner_to_workers_cq
 * automatically when the owner thread calls it. Every other thread routes to
 * workers_to_owner_cq. The check of the thread identity is the routing
 * mechanism, and it costs nothing. The caller needs no direction parameter. */
ccol_retval_t ccol_chan_send_zc(ccol_channel *ch, c_message_t *msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_send_zc(ch->owner_to_workers_cq, msg);
  }

  return ccol_circq_send_zc(ch->workers_to_owner_cq, msg);
}

/* A send on the ccol_channel that does not block. It routes to the correct
 * direction automatically. */
ccol_retval_t ccol_chan_try_send_zc(ccol_channel *ch, c_message_t *msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_try_send_zc(ch->owner_to_workers_cq, msg);
  }

  return ccol_circq_try_send_zc(ch->workers_to_owner_cq, msg);
}

/* A send on the ccol_channel with a timeout. It routes to the correct
 * direction automatically. */
ccol_retval_t ccol_chan_timed_send_zc(ccol_channel *ch, c_message_t *msg,
                                      uint64_t timeout_us) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_timed_send_zc(ch->owner_to_workers_cq, msg, timeout_us);
  }

  return ccol_circq_timed_send_zc(ch->workers_to_owner_cq, msg, timeout_us);
}

/* A receive on the ccol_channel that blocks. It routes to the correct
 * direction automatically. The owner thread receives from
 * workers_to_owner_cq. A worker thread receives from owner_to_workers_cq. */
ccol_retval_t ccol_chan_recv_zc(ccol_channel *ch, c_message_t *target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_recv_zc(ch->workers_to_owner_cq, target_buf);
  }

  return ccol_circq_recv_zc(ch->owner_to_workers_cq, target_buf);
}

/* A receive on the ccol_channel that does not block. It routes to the
 * correct direction automatically. */
ccol_retval_t ccol_chan_try_recv_zc(ccol_channel *ch, c_message_t *target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_try_recv_zc(ch->workers_to_owner_cq, target_buf);
  }

  return ccol_circq_try_recv_zc(ch->owner_to_workers_cq, target_buf);
}

/* A receive on the ccol_channel with a timeout. It routes to the correct
 * direction automatically. */
ccol_retval_t ccol_chan_timed_recv_zc(ccol_channel *ch, c_message_t *target_buf,
                                      uint64_t timeout_us) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_timed_recv_zc(ch->workers_to_owner_cq, target_buf,
                                    timeout_us);
  }

  return ccol_circq_timed_recv_zc(ch->owner_to_workers_cq, target_buf,
                                  timeout_us);
}

/* Turns off the send on the given direction. That direction is
 * ccol_owner_to_workers or ccol_workers_to_owner. This function needs an
 * explicit direction, because the caller can want to turn off only one side
 * of the ccol_channel.
 */
ccol_retval_t ccol_chan_disable_sending(ccol_channel *ch,
                                        ccol_channel_direction d) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (d == ccol_owner_to_workers) {
    return ccol_circq_disable_sending(ch->owner_to_workers_cq);
  } else if (d == ccol_workers_to_owner) {
    return ccol_circq_disable_sending(ch->workers_to_owner_cq);
  }

  return ccol_invalid_args;
}

/* Turns the send on the given direction of the ccol_channel on again. */
ccol_retval_t ccol_chan_enable_sending(ccol_channel *ch,
                                       ccol_channel_direction d) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (d == ccol_owner_to_workers) {
    return ccol_circq_enable_sending(ch->owner_to_workers_cq);
  } else if (d == ccol_workers_to_owner) {
    return ccol_circq_enable_sending(ch->workers_to_owner_cq);
  }

  return ccol_invalid_args;
}

/* Returns the message count of the circular queue below the given direction.
 * It returns ccol_invalid_size for a direction that it does not know. */
size_t ccol_chan_msg_count(ccol_channel *ch, ccol_channel_direction d) {
  if (!ch) {
    return ccol_invalid_size;
  }

  if (d == ccol_owner_to_workers) {
    return ccol_circq_msg_count(ch->owner_to_workers_cq);
  } else if (d == ccol_workers_to_owner) {
    return ccol_circq_msg_count(ch->workers_to_owner_cq);
  }

  return ccol_invalid_size;
}

/* The section about ccol_select starts here. */

/* Splices node out of a doubly-linked waiter list. The caller must already
 * hold the mutex of the owning queue. Every caller joins the splice with a
 * forward of the wake in the same critical section: a listener that leaves a
 * ready queue hands on a wake that it may hold. head and rotor must be the
 * addresses of the correct pair in the owning queue. That pair is
 * sel_read_waiters_head with sel_read_rotor, or sel_write_waiters_head with
 * sel_write_rotor.
 *
 * rotor must be the address of the round-robin cursor of the SAME list. See
 * the doc comment of notify_one_sel_waiter. If node is the current target of
 * the rotor, the function advances the rotor to the .next of node BEFORE it
 * splices node out. The rotor becomes NULL when node has no .next, and the
 * cycle then starts at *head again. This step is required, not optional.
 * After the unlink, node can be freed at once. That happens for a
 * ccol_event_loop registration. Or node can go out of scope at once. That
 * happens for a ccol_select() waiter, because the code frees its heap node
 * array when that call returns. A rotor that still points at node is
 * therefore a use-after-free, or a read of invalid stack memory, the next
 * time that notify_one_sel_waiter dereferences it. */
static void _sel_unlink_waiter_locked(ccol_sel_waiter *node,
                                      ccol_sel_waiter **head,
                                      ccol_sel_waiter **rotor) {
  if (*rotor == node) *rotor = node->next;
  if (node->prev)
    node->prev->next = node->next;
  else
    *head = node->next;
  if (node->next) node->next->prev = node->prev;
}

/* The definitions of both functions sit beside the ccol_event_loop code
 * further below, where their other callers are. They are forward declared
 * here, because deregister_sel_waiter needs them. The first one resolves a
 * queue selectable to the mutex of the owning queue and to the head and rotor
 * pair that its direction uses. The second one answers whether that direction
 * is ready right now. The caller holds the mutex of the queue in both cases.
 * The code calls them and does not restate either of them here. A waiter
 * that leaves therefore uses one single idea of "ready". Every producer,
 * scan and cascade step in this file uses that same idea. */
static void _queue_sel_locate(ccol_selectable *sel, ccol_mutex_t **out_mtx,
                              ccol_sel_waiter ***out_head,
                              ccol_sel_waiter ***out_rotor);
static bool _queue_sel_is_ready(ccol_selectable *sel);

/* Removes the waiter node at index i from the waiter list of its queue. The
 * function takes the mutex of the queue itself and unlocks it again. It sets
 * nodes[i].sel_mtx to NULL, which marks the slot as deregistered. A later
 * call to deregister_all_sel_waiters then skips that slot safely.
 *
 * The function forwards the wake, in the same critical section as the splice,
 * whenever the selectable is still ready while this node leaves the list. A
 * producer wakes exactly ONE select waiter for each message that it sends and
 * for each slot that it frees. notify_one_sel_waiter does that wake. The
 * eventfd drain below discards a notification that landed on this node after
 * its owner already stopped the wait.
 *
 * Without the forward, a wake to a waiter that then abandons its wait for an
 * unrelated reason is consumed and never replaced. That reason can be an
 * elapsed deadline, another selectable in the same array that won, one of its
 * fds that fired, or an error. A sibling waiter then stays parked while a
 * message sits in the queue. That contradicts the guarantee that no live
 * listener is ever passed over indefinitely, which the documentation of
 * ccol_event_loop_add states. It is also not the contract that a receive on
 * an empty queue blocks, because the queue is not empty.
 *
 * The forward runs after the splice, so it can never land back on this
 * departing node. The function _sel_unlink_waiter_locked already moved the
 * rotor off that node. The rotor therefore still names a node that is
 * linked. */
static void deregister_sel_waiter(size_t i, ccol_sel_waiter *nodes,
                                  ccol_selectable *selectables) {
  /* An fd selectable has no waiter list. There is nothing to unlink. */
  if (selectables[i].type == ccol_selectable_fd) return;

  ccol_mutex_t *q_mtx;
  ccol_sel_waiter **head;
  ccol_sel_waiter **rotor;
  _queue_sel_locate(&selectables[i], &q_mtx, &head, &rotor);

  ccol_mutex_lock(*q_mtx);
  _sel_unlink_waiter_locked(&nodes[i], head, rotor);
  if (_queue_sel_is_ready(&selectables[i])) notify_one_sel_waiter(head, rotor);
  ccol_mutex_unlock(*q_mtx);

  /* The code drains the eventfd and does not close it, because the next
   * iteration uses the same eventfd again. EFD_NONBLOCK is set, so this read
   * returns EAGAIN when the counter is already 0. That happens when the
   * producer wrote while this thread handled another event. The queue mutex
   * serialises the drain and the write of the producer, so the two cannot
   * race. */
  if (nodes[i].efd >= 0) {
    _eventfd_drain(nodes[i].efd);
  }
  nodes[i].sel_mtx = NULL;
}

/* Deregisters every node in the array whose sel_mtx is not NULL. */
static void deregister_all_sel_waiters(size_t n, ccol_sel_waiter *nodes,
                                       ccol_selectable *selectables) {
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].sel_mtx != NULL) {
      deregister_sel_waiter(i, nodes, selectables);
    }
  }
}

/* Resolves which of the two internal queues of a ccol_channel the calling
 * thread must watch. It uses the identity of the thread and the direction
 * that the caller asks for.
 *
 * For ccol_select_read (the receive direction):
 *   owner  reads from workers_to_owner_cq   (the same as ccol_chan_recv_zc)
 *   worker reads from owner_to_workers_cq
 *
 * For ccol_select_write (the send direction):
 *   owner  writes to owner_to_workers_cq   (the same as ccol_chan_send_zc)
 *   worker writes to workers_to_owner_cq
 */
ccol_selectable ccol_selectable_from_chan(ccol_channel *ch,
                                          ccol_select_dir dir) {
  if (!ch) {
    return (ccol_selectable){
        .type = ccol_selectable_circq, .dir = dir, .cq = NULL};
  }
  bool is_owner = (ccol_get_thread_id() == ch->owner_tid);
  ccol_circular_queue *cq;
  if (dir == ccol_select_read) {
    cq = is_owner ? ch->workers_to_owner_cq : ch->owner_to_workers_cq;
  } else {
    cq = is_owner ? ch->owner_to_workers_cq : ch->workers_to_owner_cq;
  }
  return (ccol_selectable){.type = ccol_selectable_circq, .dir = dir, .cq = cq};
}

/* Allocates an eventfd for nodes[i] when that node has none yet. It registers
 * the eventfd with epfd under EPOLLIN. The code allocates the eventfd once and
 * uses it again in each loop iteration. A later call with nodes[i].efd >= 0
 * does nothing. Returns true on success. It returns false on any system error
 * from eventfd or from epoll_ctl. */
static bool _sel_ensure_efd(size_t i, ccol_sel_waiter *nodes, int epfd) {
  if (nodes[i].efd >= 0) return true;
  nodes[i].efd = ccol_wakefd_create();
  if (nodes[i].efd < 0) return false;
  ccol_poll_event ev = {.data.u64 = (uint64_t)i, .events = CCOL_POLL_IN};
  if (ccol_poll_ctl(epfd, CCOL_POLL_CTL_ADD, nodes[i].efd, &ev) < 0) {
    close(nodes[i].efd);
    nodes[i].efd = -1;
    return false;
  }
  return true;
}

/* Fills in nodes[i] and prepends it to *head. The caller holds the mutex of
 * the owning queue, and it unlocks that mutex after this function returns. */
static void _sel_link_waiter(size_t i, ccol_sel_waiter *nodes,
                             ccol_mutex_t *sel_mtx, ccol_cond_var_t *sel_cond,
                             bool *ready, ccol_sel_waiter **head) {
  nodes[i].sel_mtx = sel_mtx;
  nodes[i].sel_cond = sel_cond;
  nodes[i].ready = ready;
  nodes[i].fwd_hops = 0;
  nodes[i].prev = NULL;
  nodes[i].next = *head;
  if (*head) (*head)->prev = &nodes[i];
  *head = &nodes[i];
}

/* Returns ccol_success when every argument is valid. In every other case it
 * returns ccol_invalid_args.
 */
static ccol_retval_t _sel_validate_args(const size_t *ready_index, size_t n,
                                        const ccol_selectable *selectables) {
  if (!ready_index || n == 0 || !selectables) return ccol_invalid_args;
  /* ccol_select_timed() holds one waiter node for each selectable. It gets
   * that array from one plain malloc() of n * sizeof(ccol_sel_waiter) bytes,
   * a few lines below its own call into this function. That malloc() does not
   * check for overflow. This check therefore guards the multiplication
   * itself, before any allocation starts. verify_circular_queue_create_inputs
   * and ccol_event_loop_create_with_mprocs already use the same
   * SIZE_MAX / element_size idiom for the same class of allocation in this
   * file. This check also covers the n_fd * sizeof(_sel_fd_pair) allocation
   * of _sel_setup_epoll, because n_fd <= n and
   * sizeof(_sel_fd_pair) <= sizeof(ccol_sel_waiter). That function also
   * carries its own guard, and it does not depend on this one alone. */
  if (n > SIZE_MAX / sizeof(ccol_sel_waiter)) return ccol_invalid_args;
  /* _sel_phase1_scan_register narrows the index of a matched selectable into
   * a plain `int`. That index is a size_t in the range 0 to n-1. The `int` is
   * its `found` local, which also carries the sentinels -1 for "nothing found
   * yet" and -2 for "system error". ccol_select_timed then widens the value
   * back with `(size_t)found`. An n that is large enough to give a match
   * index above INT_MAX narrows there to a negative or wrapped value, which
   * corrupts *ready_index. The SIZE_MAX / sizeof(ccol_sel_waiter) guard above
   * does not cover this, because that bound is many orders of magnitude
   * larger than INT_MAX on any 64-bit platform.
   * ccol_event_loop_create_with_mprocs rejects an n > INT_MAX for
   * max_events_per_wait in the same way. It guards the same class of
   * narrowing from size_t into int. */
  if (n > (size_t)INT_MAX) return ccol_invalid_args;
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

/* Computes an absolute CLOCK_MONOTONIC deadline from timeout_us. For any
 * value but UINT64_MAX, the function fills *deadline and returns true; a
 * value too large for the clock saturates. For UINT64_MAX, it returns false,
 * which means a wait with no end. */
static bool _sel_compute_deadline(uint64_t timeout_us,
                                  struct timespec *deadline) {
  if (timeout_us == UINT64_MAX) return false;
  ccol_deadline_after_us(timeout_us, deadline);
  return true;
}

/* The base epoll interest flags for one direction on an fd selectable. The
 * function _sel_setup_epoll uses them for the registration. The function
 * _sel_wait_epoll uses them to resolve a combined fd event that fired. It
 * resolves that event back to the one member selectable that it satisfies.
 * Both call this function, so the two can never disagree with each other. */
static uint32_t _sel_fd_base_events(ccol_select_dir dir) {
  return (dir == ccol_select_read)
             ? (uint32_t)(CCOL_POLL_IN | CCOL_POLL_RDHUP | CCOL_POLL_ERR |
                          CCOL_POLL_HUP)
             : (uint32_t)(CCOL_POLL_OUT | CCOL_POLL_ERR | CCOL_POLL_HUP);
}

/* A tag bit for the ev.data.u64 of the epoll registration of an fd group. The
 * real fd value sits in the other 63 bits. This tag never collides with the
 * ev.data.u64 of a queue selectable. That value is a plain index into
 * selectables[] and is always smaller than n. See _sel_ensure_efd. A real n
 * can never come near 2^63, because the caller would first need a real
 * selectables[] array with that many elements. The tag is what lets
 * _sel_wait_epoll tell an fd that fired from a queue eventfd that fired. It
 * reads only the ev.data.u64 value and needs no separate side table. */
#define _SEL_FD_TAG ((uint64_t)1 << 63)

/* One fd selectable together with its own index into selectables[]. The code
 * sorts an array of these by fd. Every selectable that shares one real fd
 * then sorts into one contiguous run. Those selectables can mix directions,
 * and they can be exact duplicates. See _sel_setup_epoll. */
typedef struct {
  int fd;
  size_t idx;
} _sel_fd_pair;

static int _sel_fd_pair_cmp(const void *a, const void *b) {
  const _sel_fd_pair *pa = (const _sel_fd_pair *)a;
  const _sel_fd_pair *pb = (const _sel_fd_pair *)b;
  if (pa->fd != pb->fd) return (pa->fd < pb->fd) ? -1 : 1;
  if (pa->idx != pb->idx) return (pa->idx < pb->idx) ? -1 : 1;
  return 0;
}

/* Creates an epoll instance. It registers every fd selectable with
 * level-triggered interest flags. Some selectables share the same fd. One
 * example is a caller that watches one connected socket for readability and
 * for writability in a single call. Another example is two selectables that
 * are exact duplicates. The code combines all of them into a SINGLE epoll_ctl
 * registration. That registration carries the OR of the interest mask of
 * every member. ccol_event_loop_add already combines a read registration and
 * a write registration that share one fd into one entry in the same way.
 * Without this combination, a second EPOLL_CTL_ADD for an fd that an earlier
 * step of the same call registered fails with EEXIST. That is not an error of
 * the caller, because nothing in the public API forbids it or documents it as
 * unsupported. It is also a common, ordinary request: wait for either
 * direction on one fd, whichever comes first.
 *
 * The code groups the selectables by fd with a sort. qsort costs O(n log n).
 * The other design is a linear scan for each lookup of "is this fd already
 * registered", and each of those scans costs O(n). The sort keeps the common
 * case away from O(n^2). That common case is many distinct fds with no
 * sharing at all. This project treats algorithmic complexity that can be
 * reduced as a bug, even when the typical n of today is small.
 *
 * Returns the epfd on success. It returns -1 on any system error. A close of
 * the returned epfd removes every registered fd automatically. */
static int _sel_setup_epoll(size_t n, ccol_selectable *selectables) {
  int epfd = ccol_poll_create();
  if (epfd < 0) return -1;

  size_t n_fd = 0;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == ccol_selectable_fd) n_fd++;
  }
  if (n_fd == 0) return epfd;

  /* This guards the n_fd * sizeof(_sel_fd_pair) multiplication below against
   * a size_t overflow. The n * sizeof(ccol_sel_waiter) guard of
   * _sel_validate_args already makes this case unreachable, because
   * n_fd <= n and sizeof(_sel_fd_pair) <= sizeof(ccol_sel_waiter). The code
   * keeps this guard anyway, as an explicit local check at the real point of
   * allocation. It does not depend only on a proof that comes from a distant
   * caller. This file uses the same SIZE_MAX / element_size idiom in other
   * places. */
  if (n_fd > SIZE_MAX / sizeof(_sel_fd_pair)) {
    close(epfd);
    return -1;
  }

  _sel_fd_pair *pairs = malloc(n_fd * sizeof(_sel_fd_pair));
  if (!pairs) {
    close(epfd);
    return -1;
  }
  size_t n_pairs = 0;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type != ccol_selectable_fd) continue;
    pairs[n_pairs].fd = selectables[i].fd;
    pairs[n_pairs].idx = i;
    n_pairs++;
  }
  qsort(pairs, n_pairs, sizeof(_sel_fd_pair), _sel_fd_pair_cmp);

  for (size_t i = 0; i < n_pairs;) {
    int fd = pairs[i].fd;
    uint32_t combined = 0;
    size_t j = i;
    while (j < n_pairs && pairs[j].fd == fd) {
      combined |= _sel_fd_base_events(selectables[pairs[j].idx].dir);
      j++;
    }

    ccol_poll_event ev;
    ev.data.u64 = _SEL_FD_TAG | (uint64_t)(unsigned int)fd;
    ev.events = combined;
    if (ccol_poll_ctl(epfd, CCOL_POLL_CTL_ADD, fd, &ev) < 0) {
      free(pairs);
      close(epfd);
      return -1;
    }
    i = j;
  }

  free(pairs);
  return epfd;
}

/* Scans every selectable that is not an fd and checks it for readiness. For
 * each selectable that is not ready yet, it links a waiter node into the list
 * of the queue. It returns the index that it found, which is 0 or larger, when
 * it finds a ready selectable and consumes its resource or confirms its slot.
 * It returns -1 when no selectable is ready and every waiter is now
 * registered. It returns -2 on a system error from eventfd or from epoll_ctl.
 * On -2 the code already unlocked the mutex of the queue that failed. The
 * nodes that it registered before stay linked, and the caller must call
 * deregister_all_sel_waiters before it frees them. */
static int _sel_phase1_scan_register(size_t n, ccol_selectable *selectables,
                                     ccol_sel_waiter *nodes, bool has_fd_sels,
                                     int epfd, ccol_mutex_t *sel_mtx,
                                     ccol_cond_var_t *sel_cond, bool *ready) {
  int found = -1;
  for (size_t i = 0; i < n && found < 0; i++) {
    if (selectables[i].type == ccol_selectable_fd) continue;

    if (selectables[i].type == ccol_selectable_circq) {
      ccol_circular_queue *cq = selectables[i].cq;
      ccol_mutex_lock(cq->mutex);

      if (selectables[i].dir == ccol_select_read) {
        /* This is a peek only. ccol_select() never consumes a message. The
         * caller does its own explicit ccol_circq_try_recv_zc() after the
         * call. The code forwards the notify to the next waiter every time.
         * This matches the write-direction branch below. The forward does not
         * depend on messages that remain after a consume, because this
         * function never consumes. A thread that is about to leave the waiter
         * list has no other way to tell the next waiter. The next waiter must
         * learn that the condition is still true. Without this forward, the
         * read direction has the same class of starvation that
         * write_circq_two_concurrent_waiters_both_wake_on_slot_free guards
         * against on the write-direction side. */
        if (cq->msg_count > 0) {
          notify_one_sel_waiter(&cq->sel_read_waiters_head,
                                &cq->sel_read_rotor);
          ccol_mutex_unlock(cq->mutex);
          found = (int)i;
        } else {
          if (has_fd_sels && !_sel_ensure_efd(i, nodes, epfd)) {
            ccol_mutex_unlock(cq->mutex);
            return -2;
          }
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &cq->sel_read_waiters_head);
          ccol_mutex_unlock(cq->mutex);
        }
      } else {
        /* ccol_select_write: writable when there is room and the send is on */
        if (cq->msg_count < cq->max_size && !cq->writing_disabled) {
          notify_one_sel_waiter(&cq->sel_write_waiters_head,
                                &cq->sel_write_rotor);
          ccol_mutex_unlock(cq->mutex);
          found = (int)i;
        } else {
          if (has_fd_sels && !_sel_ensure_efd(i, nodes, epfd)) {
            ccol_mutex_unlock(cq->mutex);
            return -2;
          }
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &cq->sel_write_waiters_head);
          ccol_mutex_unlock(cq->mutex);
        }
      }
    } else {
      /* ccol_selectable_dynq */
      ccol_dynamic_queue *dq = selectables[i].dq;
      ccol_mutex_lock(dq->mutex);

      if (selectables[i].dir == ccol_select_read) {
        /* This is a peek only. The reason is the same as in the circq read
         * branch above. */
        if (dq->msg_count > 0) {
          notify_one_sel_waiter(&dq->sel_read_waiters_head,
                                &dq->sel_read_rotor);
          ccol_mutex_unlock(dq->mutex);
          found = (int)i;
        } else {
          if (has_fd_sels && !_sel_ensure_efd(i, nodes, epfd)) {
            ccol_mutex_unlock(dq->mutex);
            return -2;
          }
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &dq->sel_read_waiters_head);
          ccol_mutex_unlock(dq->mutex);
        }
      } else {
        /* ccol_select_write: writable when writing_disabled is false and
         * the queue is not at capacity */
        if (!dq->writing_disabled && dq->msg_count < ccol_max_elem_count) {
          notify_one_sel_waiter(&dq->sel_write_waiters_head,
                                &dq->sel_write_rotor);
          ccol_mutex_unlock(dq->mutex);
          found = (int)i;
        } else {
          if (has_fd_sels && !_sel_ensure_efd(i, nodes, epfd)) {
            ccol_mutex_unlock(dq->mutex);
            return -2;
          }
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &dq->sel_write_waiters_head);
          ccol_mutex_unlock(dq->mutex);
        }
      }
    }
  }
  return found;
}

typedef enum {
  _SEL_CONDVAR_READY,     /* A normal wake, or a wait with no end. Go on to
                            Phase 3. */
  _SEL_CONDVAR_TIMED_OUT, /* The deadline elapsed and *ready is still false */
  _SEL_CONDVAR_FAILURE,   /* ccol_cond_var_timedwait returned an error that is
                            not 0 and not ETIMEDOUT */
} _sel_condvar_outcome;

#ifdef RUNNING_UNIT_TESTS
/* A test-only hook. It forces the next ccol_cond_var_timedwait call in the
 * deadline branch of _sel_wait_condvar to report EINVAL. That call then does
 * no real wait. The hook disarms itself after that. It exists so that a test
 * can reach one path every time. That path is the one where
 * ccol_cond_var_timedwait returns an unexpected error that is not 0 and not
 * ETIMEDOUT. No correct call from this codebase can trigger a real EINVAL
 * here. _sel_compute_deadline always keeps tv_nsec in [0, 1e9). It does this
 * before the code hands the deadline to ccol_cond_var_timedwait. */
static _Atomic bool g_sel_force_condvar_wait_error = false;

/* A test-only hook. The code reads it together with
 * g_sel_force_condvar_wait_error. When both fire together, the injected error
 * also sets *ready = true directly before the code reports the error. This
 * simulates a producer that finished its notify under the same sel_mtx that
 * this call already holds. That producer finished a moment before the code
 * sees the unrelated, forced error. This is the only way for a test to reach
 * that interleave every time. The hook replaces the real
 * ccol_cond_var_timedwait call and skips it, so the code does not unlock
 * sel_mtx. The _notify_waiter of a truly concurrent producer thread needs
 * sel_mtx, so it can never race in here on its own. See the FAILURE-branch
 * comment of _sel_wait_condvar for the reason why *ready must win over an
 * unexpected error at the same moment. */
static _Atomic bool g_sel_force_condvar_wait_error_also_marks_ready = false;

void ccol_select_test_force_next_condvar_wait_error(void) {
  atomic_store(&g_sel_force_condvar_wait_error, true);
}

void ccol_select_test_force_next_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_sel_force_condvar_wait_error, true);
  atomic_store(&g_sel_force_condvar_wait_error_also_marks_ready, true);
}

/* A test-only hook. The condvar-only wait of ccol_select_timed reports that
 * its deadline elapsed. The Phase 3 deregister step of that same call comes
 * after it. This hook widens the window between the two. That window is
 * normally only a few instructions long, and the hook makes it as long as the
 * test needs. A notify from a producer that lands in exactly that window
 * targets a waiter that already stopped its wait. The forward in
 * deregister_sel_waiter exists to cover that interleave. In production the
 * window is far too narrow to hit on demand. The hook fires once and then
 * disarms itself. It also keeps a counter that a test polls, so the test
 * learns that the delay really started. The test does not guess with a
 * sleep. */
static _Atomic uint32_t g_sel_timed_out_deregister_delay_us = 0;
static _Atomic uint64_t g_sel_timed_out_deregister_delays = 0;

void ccol_select_test_delay_next_timed_out_deregister_us(uint32_t us) {
  atomic_store(&g_sel_timed_out_deregister_delay_us, us);
}

uint64_t ccol_select_test_timed_out_deregister_delay_count(void) {
  return atomic_load(&g_sel_timed_out_deregister_delays);
}

static void _sel_timed_out_deregister_test_delay(void) {
  uint32_t us = atomic_exchange(&g_sel_timed_out_deregister_delay_us, 0u);
  if (us == 0) return;
  atomic_fetch_add(&g_sel_timed_out_deregister_delays, (uint64_t)1);
  struct timespec ts = {.tv_sec = us / 1000000,
                        .tv_nsec = (long)(us % 1000000) * 1000L};
  nanosleep(&ts, NULL);
}
#endif

/* Waits on sel_cond until a producer sets *ready, or until the deadline
 * elapses. It returns _SEL_CONDVAR_TIMED_OUT when the deadline elapses and
 * *ready is still false. It returns _SEL_CONDVAR_FAILURE when
 * ccol_cond_var_timedwait returns an error that is not ETIMEDOUT and *ready
 * is still false. It returns _SEL_CONDVAR_READY on a normal wake. A wait with
 * no end always ends with _SEL_CONDVAR_READY. A wait with a deadline also
 * gives _SEL_CONDVAR_READY when *ready is already true at the moment that the
 * code sees ETIMEDOUT or an unexpected error. The function always sets *ready
 * back to false before it returns.
 *
 * The FAILURE case matches every other timed wait function in this file.
 * Those are ccol_circq_timed_send_zc, ccol_circq_timed_recv_zc and
 * ccol_dynmq_timed_recv_zc. Each of them treats a return from
 * ccol_cond_var_timedwait that is not 0 and not ETIMEDOUT as
 * ccol_unexpected_failure, and none of them retries. Without a separate exit
 * for that error, the only exits of the loop are ETIMEDOUT and *ready. Such
 * an error then enters ccol_cond_var_timedwait again with the same deadline.
 * The loop spins forever and never returns to the caller.
 *
 * The FAILURE branch checks *ready again before it commits to FAILURE. The
 * ETIMEDOUT branch directly above it does the same check.
 * ccol_cond_var_timedwait always takes *sel_mtx again before it returns, on
 * success and on failure. The _notify_waiter of a producer needs *sel_mtx to
 * set *ready = true. That producer can therefore finish and hand the mutex
 * back to this exact call. It can do this a moment before the code also sees
 * an unrelated, spurious error that is not ETIMEDOUT. Without this second
 * check, the code discards that real wake. It then reports
 * ccol_unexpected_failure to the caller, although the operation succeeded. */
static _sel_condvar_outcome _sel_wait_condvar(ccol_mutex_t *sel_mtx,
                                              ccol_cond_var_t *sel_cond,
                                              bool *ready, bool has_deadline,
                                              const struct timespec *deadline) {
  ccol_mutex_lock(*sel_mtx);
  if (has_deadline) {
    _sel_condvar_outcome outcome = _SEL_CONDVAR_READY;
    while (!*ready) {
      int wait_ret;
#ifdef RUNNING_UNIT_TESTS
      if (atomic_load(&g_sel_force_condvar_wait_error)) {
        atomic_store(&g_sel_force_condvar_wait_error, false);
        wait_ret = EINVAL;
        if (atomic_load(&g_sel_force_condvar_wait_error_also_marks_ready)) {
          atomic_store(&g_sel_force_condvar_wait_error_also_marks_ready, false);
          *ready = true;
        }
      } else {
        wait_ret = ccol_cond_var_timedwait(*sel_cond, *sel_mtx, *deadline);
      }
#else
      wait_ret = ccol_cond_var_timedwait(*sel_cond, *sel_mtx, *deadline);
#endif
      if (wait_ret == ETIMEDOUT) {
        if (!*ready) outcome = _SEL_CONDVAR_TIMED_OUT;
        break;
      }
      if (wait_ret != 0) {
        if (!*ready) outcome = _SEL_CONDVAR_FAILURE;
        break;
      }
    }
    *ready = false;
    ccol_mutex_unlock(*sel_mtx);
    return outcome;
  }
  while (!*ready) ccol_cond_var_wait(*sel_cond, *sel_mtx);
  *ready = false;
  ccol_mutex_unlock(*sel_mtx);
  return _SEL_CONDVAR_READY;
}

typedef enum {
  _SEL_EPOLL_CONTINUE, /* A queue eventfd fired. Go on to Phase 3. */
  _SEL_EPOLL_BREAK,    /* Done or timed out. *out_retval and *ready_index are
                          set. */
  _SEL_EPOLL_FAILURE,  /* An unexpected system error. The nodes stay
                          registered. */
} _sel_epoll_outcome;

/* Blocks on epoll_wait until one registered descriptor is ready, or until the
 * deadline elapses. It returns _SEL_EPOLL_CONTINUE when a queue eventfd
 * fires. The caller then runs Phase 3 and goes back to Phase 1. It returns
 * _SEL_EPOLL_BREAK when the whole result is known, and it sets *out_retval
 * and *ready_index itself. It returns _SEL_EPOLL_FAILURE on a system error.
 * The nodes then stay registered, and the caller must deregister them before
 * it frees them. */
static _sel_epoll_outcome _sel_wait_epoll(
    int epfd, bool has_deadline, const struct timespec *deadline, size_t n,
    ccol_selectable *selectables, ccol_sel_waiter *nodes, size_t *ready_index,
    ccol_retval_t *out_retval) {
  ccol_poll_event ev;
  int n_ready;
  int epoll_to;
  do {
    if (has_deadline) {
      /* The remaining time rounds UP to the next whole millisecond, so that
       * the wait never ends before the deadline. Once the deadline has
       * passed, the timeout is 0 and epoll_wait still runs: a descriptor
       * that is already ready is reported, and an epoll timeout of 0 is a poll
       * that does not block, never a timeout without a look. */
      epoll_to = ccol_deadline_remaining_ms_ceil(deadline);
    } else {
      epoll_to = -1;
    }
    n_ready = ccol_poll_wait(epfd, &ev, 1, epoll_to);
  } while (n_ready < 0 && errno == EINTR);

  if (n_ready == 0) {
    deregister_all_sel_waiters(n, nodes, selectables);
    *out_retval = ccol_timed_out;
    return _SEL_EPOLL_BREAK;
  }
  if (n_ready < 0) return _SEL_EPOLL_FAILURE;

  if (ev.data.u64 & _SEL_FD_TAG) {
    int fd = (int)(ev.data.u64 & ~_SEL_FD_TAG);
    /* The fd belongs to the caller for the whole call, so it is open. */
    ccol_poll_refine(&ev, fd);
    deregister_all_sel_waiters(n, nodes, selectables);

    /* One combined registration can stand for several selectables that share
     * this fd. See _sel_setup_epoll. The code picks the first one in the
     * array order of the caller whose own direction the events from the
     * kernel satisfy. This search always finds one. ev.events is a subset of
     * the combined mask for fd, and that mask is the OR of the base mask of
     * every member. Every bit that is set in ev.events therefore comes from
     * the base mask of at least one member, and it matches that member. */
    size_t match = n;
    for (size_t k = 0; k < n; k++) {
      if (selectables[k].type != ccol_selectable_fd) continue;
      if (selectables[k].fd != fd) continue;
      if (_sel_fd_base_events(selectables[k].dir) & ev.events) {
        match = k;
        break;
      }
    }
    ccol_assert(match < n);

    /* This reports readiness only, for both directions. ccol_select() never
     * reads the fd and never writes to it. The caller does its own read(2),
     * recv(2), write(2) or send(2) after the call. */
    *out_retval = ccol_success;
    *ready_index = match;
    return _SEL_EPOLL_BREAK;
  }
  /* A queue eventfd fired. Go on to Phase 3. */
  return _SEL_EPOLL_CONTINUE;
}

/* Blocks until at least one of the n selectables is ready, or until
 * timeout_us elapses. It then sets *ready_index. ccol_select_timed() never
 * does the receive or the send itself, for any type of selectable. The caller
 * does that explicitly after the call. A timeout_us of UINT64_MAX means a
 * wait with no end.
 *
 * The lock protocol prevents a deadlock and a lost wake. Each iteration has
 * three phases.
 *
 *   Phase 1: for each queue, under the mutex of that queue, one at a time:
 *     Read direction: check msg_count > 0. This is a peek only, and it
 *       consumes nothing. If the queue is ready, forward the notify to the
 *       next read waiter and mark the selectable as found. That forward
 *       matches the write-direction cascade below. If the queue is not ready,
 *       prepend a waiter node to the sel_read_waiters_head list of the queue
 *       and wait. The read direction waits whatever the writing_disabled
 *       state is.
 *     Write direction: for a ccol_circular_queue, check
 *       msg_count < max_size && !writing_disabled. For a ccol_dynamic_queue,
 *       check !writing_disabled && msg_count < ccol_max_elem_count. If the
 *       queue is writable, mark the selectable as found at once. Nothing is
 *       reserved and nothing is consumed. If the queue is not writable,
 *       prepend a waiter node to sel_write_waiters_head. There is no terminal
 *       state, so the wait goes on.
 *     An fd selectable goes directly into the epoll set. See below.
 *
 *   Phase 2: the wait:
 *     With no fd selectable present, the thread blocks on sel_cond under
 *       sel_mtx. The while loop guards against a spurious wake. It also
 *       guards against a signal that fired between Phase 1 and the wait. A
 *       producer sets the ready flag under sel_mtx before it signals, so no
 *       wake can be lost. With a deadline, the code uses
 *       ccol_cond_var_timedwait on a CLOCK_MONOTONIC condition variable.
 *       ETIMEDOUT breaks the loop, and the function returns ccol_timed_out.
 *     With an fd selectable present, the thread blocks on epoll_wait(epfd).
 *       Each queue waiter carries its own eventfd, which has efd >= 0.
 *       notify_sel_waiters writes 1 to that eventfd and also signals
 *       sel_cond, so epoll_wait wakes for a queue event and for an fd event.
 *       If a user fd fires, the function returns at once. If a queue eventfd
 *       fires, the code goes on to Phase 3 and looks at the state again. With
 *       a deadline, the code computes the time that remains before each
 *       epoll_wait call, and it does this after an EINTR retry too. This is
 *       why an EINTR cannot extend the timeout. An epoll_wait that returns 0
 *       means that the deadline elapsed, and the function returns
 *       ccol_timed_out.
 *
 *   Phase 3: the deregister step:
 *     Take the mutex of each queue again. Splice the node out of the correct
 *     waiter list. Forward the wake to the next waiter when that queue is
 *     still ready; see deregister_sel_waiter. Drain each open eventfd with a
 *     read that does not block, which sets its counter back to 0 for the next
 *     iteration. Then go back to Phase 1.
 *
 * The lock order is the queue mutex first and sel_mtx second. Every path in
 * this file uses that one order. A producer takes it to notify. Phase 1 takes
 * it when a ready queue forwards its wake. Phase 3 takes it when a node
 * leaves. The condvar path of Phase 2 takes sel_mtx alone and never takes a
 * queue mutex below it. epoll_wait holds no application lock. There is
 * therefore no cycle in the lock order.
 *
 * Node lifetime: a producer notifies while it holds the queue mutex. The
 * deregister step also needs the queue mutex. A node can therefore not leave
 * the list while a producer walks that list, and the heap node cannot vanish
 * during a walk.
 *
 * The epfd lifecycle: the code creates the epfd once for each call, before
 * the loop. It adds each user fd once with EPOLL_CTL_ADD before the loop, and
 * those fds stay registered for the whole call. The level-triggered semantics
 * make a ready fd fire on every epoll_wait until the caller consumes it. The
 * close of epfd on return removes them automatically, so the code needs no
 * EPOLL_CTL_DEL and no EPOLL_CTL_ADD for each iteration. The code creates a
 * queue eventfd for each waiter at its first registration and uses it again
 * in each iteration. deregister_sel_waiter drains those eventfds with a read
 * that does not block, and it does not close them. Phase 1 therefore links
 * the same node again with no new epoll_ctl and no new eventfd syscall. */
ccol_retval_t ccol_select_timed(size_t *ready_index, size_t n,
                                ccol_selectable *selectables,
                                uint64_t timeout_us) {
  ccol_retval_t v = _sel_validate_args(ready_index, n, selectables);
  if (v != ccol_success) return v;

  bool has_fd_sels = false;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == ccol_selectable_fd) {
      has_fd_sels = true;
      break;
    }
  }

  /* One waiter node for each selectable. sel_mtx == NULL means that the node
   * is not registered in the list of any queue now. efd == -1 means that the
   * node has no eventfd, which is the condvar-only mode. The array is on the
   * heap, to prevent a stack overflow when n is large.
   */
  ccol_sel_waiter *nodes = malloc(n * sizeof(ccol_sel_waiter));
  if (!nodes) return ccol_not_enough_memory;
  for (size_t i = 0; i < n; i++) {
    nodes[i].sel_mtx = NULL;
    nodes[i].efd = -1;
  }

  ccol_mutex_t sel_mtx;
  ccol_cond_var_t sel_cond;
  bool ready = false;
  if (ccol_mutex_init(sel_mtx) != 0) {
    free(nodes);
    return ccol_unexpected_failure;
  }
  /* CLOCK_MONOTONIC, like every other condition variable a timed wait in
   * this file blocks on; see _init_monotonic_cond_var. */
  if (_init_monotonic_cond_var(&sel_cond) != 0) {
    ccol_mutex_destroy(sel_mtx);
    free(nodes);
    return ccol_unexpected_failure;
  }

  bool has_deadline;
  struct timespec deadline = {0, 0};
  has_deadline = _sel_compute_deadline(timeout_us, &deadline);

  int epfd = -1;
  if (has_fd_sels) {
    epfd = _sel_setup_epoll(n, selectables);
    if (epfd < 0) {
      free(nodes);
      ccol_mutex_destroy(sel_mtx);
      ccol_cond_var_destroy(sel_cond);
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
      _sel_condvar_outcome outcome = _sel_wait_condvar(
          &sel_mtx, &sel_cond, &ready, has_deadline, &deadline);
      if (outcome == _SEL_CONDVAR_TIMED_OUT) {
#ifdef RUNNING_UNIT_TESTS
        _sel_timed_out_deregister_test_delay();
#endif
        deregister_all_sel_waiters(n, nodes, selectables);
        retval = ccol_timed_out;
        break;
      }
      if (outcome == _SEL_CONDVAR_FAILURE) goto cleanup_unexpected_failure;
    } else {
      _sel_epoll_outcome outcome =
          _sel_wait_epoll(epfd, has_deadline, &deadline, n, selectables, nodes,
                          ready_index, &retval);
      if (outcome == _SEL_EPOLL_BREAK) break;
      if (outcome == _SEL_EPOLL_FAILURE) goto cleanup_unexpected_failure;
      /* _SEL_EPOLL_CONTINUE: go on to Phase 3 */
    }

    /* === Phase 3: deregister, then go back to Phase 1 ===
     * deregister_sel_waiter drains each open eventfd with a read that does
     * not block, which sets its counter back to 0. The eventfd stays
     * registered in epoll, so Phase 1 links the node again with no new
     * syscall. A user fd stays registered in epoll for the same reason. */
    deregister_all_sel_waiters(n, nodes, selectables);
    /* No node is linked now, so no producer can reach ready or an eventfd of
     * this call until Phase 1 links a node again. The wake that ready
     * recorded is consumed: Phase 1 looks at the state of every queue again.
     * Clearing it here is what lets the first notify after the next link
     * deliver a wake again. The condvar wait clears it itself, but a notify
     * that lands between the return of that wait and the deregister sets it
     * again, and in epoll mode nothing else clears it at all. */
    ccol_mutex_lock(sel_mtx);
    ready = false;
    ccol_mutex_unlock(sel_mtx);
  }

  /* Close every eventfd that the iterations allocated. The close of epfd
   * removes every user fd and every queue eventfd from the epoll set
   * automatically. */
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].efd >= 0) close(nodes[i].efd);
  }
  if (epfd >= 0) close(epfd);
  ccol_mutex_destroy(sel_mtx);
  ccol_cond_var_destroy(sel_cond);
  free(nodes);
  return retval;

cleanup_unexpected_failure:
  deregister_all_sel_waiters(n, nodes, selectables);
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].efd >= 0) close(nodes[i].efd);
  }
  if (epfd >= 0) close(epfd);
  ccol_mutex_destroy(sel_mtx);
  ccol_cond_var_destroy(sel_cond);
  free(nodes);
  return ccol_unexpected_failure;
}

ccol_retval_t ccol_select(size_t *ready_index, size_t n,
                          ccol_selectable *selectables) {
  return ccol_select_timed(ready_index, n, selectables, UINT64_MAX);
}

/* The section about ccol_event_loop starts here. */

typedef struct event_entry event_entry;

/* The internal registration struct, which the code can change. The PUBLIC
 * ccol_event_reg type in cthreadcomm.h is different: it is an opaque uint64_t
 * handle. The code resolves that handle against the reg_slots table of this
 * loop before it dereferences anything. See _ccol_event_reg_resolve. The
 * public type is never this struct. */
typedef struct ccol_event_reg_s ccol_event_reg_s;
struct ccol_event_reg_s {
  ccol_selectable sel;
  ccol_event_handlers_t handlers;
  void *arg;
  _Atomic int refcount; /* 1 while registered, +1 for each live callback */
  _Atomic bool removed;

  /* True between a ccol_event_loop_pause that succeeds and the matching
   * ccol_event_loop_resume. The registration stays fully intact. It keeps its
   * slot in owning_entry->as.fd.read_reg or in owning_entry->as.fd.write_reg.
   * It still counts toward loop->reg_count, and it keeps its generation. But
   * it is not part of the combined epoll interest mask of the fd. No callback
   * therefore fires for it while this flag is true.
   * _ccol_event_loop_rearm_entry_locked reads this flag. The first mask
   * computation of _ccol_event_loop_add_fd reads it too. Both of them already
   * compute the mask again from live state, and not from a cached snapshot.
   * This flag is one more bit of that live state, not a new mechanism. The
   * flag is always false for a queue or ccol_channel registration, because
   * ccol_event_loop_pause rejects those. ccol_event_loop_modify has the same
   * restriction. */
  _Atomic bool paused;

  /* True once an event reached this fd registration that it has no handler
   * for: neither the handler of its direction nor on_error can take it. That
   * condition is typically a hang-up, which level-triggered epoll reports
   * again on every epoll_wait, so a registration that keeps being armed for
   * it makes the reactor spin. Like paused, the flag removes the
   * registration from the combined interest mask of the fd; a sibling
   * direction with handlers stays armed. The collection sets it, and
   * ccol_event_loop_modify and ccol_event_loop_resume clear it. Every read and
   * write is under the stripe lock of the registration. It sits in the
   * padding after paused, so the struct keeps its size. */
  bool muted;

  event_entry *owning_entry;

  /* Which of loop->stripes holds the entry of this registration. The code
   * sets it once, at the same point where it sets owning_entry, and it never
   * writes it again. ccol_event_loop_modify and ccol_event_loop_remove must
   * key their stripe lookup on THIS field, and not on
   * owning_entry->stripe_idx. A caller can correctly hold a stale reg*, which
   * is the exact case that the contract of _ccol_event_loop_defer_reg_free
   * exists for. The owning_entry of such a reg can be freed already, because
   * an entry and a reg have separate deferred-free lists that drain
   * independently. The deferred-free list of the reg is what makes
   * reg->stripe_idx always safe to read. The list of the entry is a different
   * guarantee, and it does not protect a read through a stale reg. */
  size_t stripe_idx;

  /* The identity token that the caller sees through
   * ccol_event_loop_reg_generation. The code copies it from
   * owning_entry->generation at the same point where it copies stripe_idx.
   * The reason is the same. A stale reg* must never read this value through
   * owning_entry, because owning_entry can be freed independently. The code
   * sets it once and never writes it again after that copy. It is safe to
   * read at any time under the deferred-free contract of the reg, like
   * stripe_idx. */
  uint64_t generation;

  /* The public ccol_event_reg handle of this registration, the value that
   * ccol_event_loop_add returns. _ccol_event_reg_slot_acquire writes it once,
   * before the code wires reg into any registry, so every dispatch reads it
   * after that write and no dispatch can see it unset. The dispatch passes
   * it to every on_readable, on_writable and on_error call. A callback can
   * therefore act on its own registration even when it runs before
   * ccol_event_loop_add has returned the handle to its caller. */
  ccol_event_reg self_handle;

  /* The index of this reg into loop->reg_slots. See the comment of the
   * reg_slots field of struct ccol_event_loop_s for the full design. The code
   * sets it once, at the same point where it sets stripe_idx and generation,
   * and it never writes it again. Only ccol_event_loop_remove uses it, to
   * invalidate the slot the moment that it removes this reg. Nothing else
   * uses it, because every OTHER lookup goes through the public
   * ccol_event_reg handle value and never through this raw struct. */
  uint32_t self_slot_idx;

  /* _ccol_event_reg_resolve pins this count with an atomic increment, while
   * it holds the read side of loop->reg_slot_rwlock. The pin stays while a
   * caller holds a ccol_event_reg_s* that it resolved and did not yet release
   * with _ccol_event_reg_resolve_unpin. The function
   * _ccol_event_loop_reclaim_pending_frees does not free a reg while this
   * count is not zero, whatever its refcount is. A call on that exact reg can
   * be in flight. That call is a ccol_event_loop_modify, _pause, _resume,
   * _remove or ccol_event_loop_reg_generation. This count is what makes a
   * free of the reg impossible for the whole duration of that call.
   * The poller_batch_gen epoch check alone leaves that gap open. See the
   * reg_slots comment of struct ccol_event_loop_s. */
  _Atomic size_t pending_resolve_count;

  int bridge_efd; /* -1 for an fd selectable. For a queue or ccol_channel
                   * selectable it is the persistent bridge eventfd. */

  /* For a queue or ccol_channel selectable only. _notify_waiter above
   * records a wake in the ready flag of a node under the mutex of that node,
   * so a persistent registration needs its own mutex, condition variable and
   * ready flag. wait_ready is true exactly while a wake is pending on
   * bridge_efd. _queue_reg_consume_wake clears it together with the drain,
   * right before a dispatch runs the callback, so that one eventfd write
   * covers every send until that dispatch. Nothing ever waits on wait_cond,
   * because a node with an eventfd gets no signal. */
  ccol_mutex_t wait_mtx;
  ccol_cond_var_t wait_cond;
  bool wait_ready;
  ccol_sel_waiter waiter_node;

  /* An intrusive list of every queue-backed reg that is registered now on the
   * stripe of this registration. An fd-backed reg needs no such list, because
   * the fd table of that stripe already reaches it. A queue selectable has no
   * equivalent table to walk. The function __ccol_event_loop_destroy must
   * find every queue-backed waiter_node. It must unlink each of them from
   * the list of its queue before it frees that node. */
  ccol_event_reg_s *loop_list_prev;
  ccol_event_reg_s *loop_list_next;

  /* The code links this reg into loop->pending_reg_frees when its refcount
   * reaches 0. See _ccol_event_loop_defer_reg_free below. Nothing uses the
   * reg again after the free starts.
   */
  ccol_event_reg_s *pending_free_next;
};

/* ev.data.ptr always points to this struct, for every epoll registration that
 * this module owns. For an fd, up to two event_regs can share one entry:
 * read_reg and write_reg. epoll_ctl keys its interest list by fd, and not by
 * the pair of fd and direction. ev.data.ptr therefore cannot hold a bare
 * ccol_event_reg* directly. With such a design, the reg of the direction that
 * registered last silently receives every event on that fd. Those events
 * include the events for the other direction. A queue or ccol_channel
 * selectable never shares an entry. Each one gets its own bridge eventfd and
 * its own entry with one reg. */
struct event_entry {
  bool is_fd;
  int fd; /* For an fd selectable only. It is also the fd-table key. */

  /* Which of loop->stripes holds this entry. The lock of that stripe protects
   * as.fd.read_reg, as.fd.write_reg and as.reg. The fd_index and the
   * queue_regs_head of that stripe hold this entry. The code sets this field
   * once at creation, before it publishes the entry. Publication means the
   * insert into the chmap of a stripe, or the epoll_ctl call. The code never
   * writes the field again, so every reader can read it with no lock. The
   * dispatch collector is such a reader, and it reads through ev->data.ptr.
   * The code must NOT derive this value later from entry->as.reg, from
   * entry->as.fd.read_reg or from entry->as.fd.write_reg outside a lock. The
   * queue branch of ccol_event_loop_remove sets entry->as.reg to NULL. The
   * poller can already hold a stale epoll batch entry. That NULL is what
   * stops the poller from a dereference of a freed reg through it. A stripe
   * key derived from that exact field at dispatch time would defeat that
   * protection. */
  size_t stripe_idx;

  /* The dispatch lock of this entry. epoll is level-triggered by default,
   * and the code uses no EPOLLEXCLUSIVE. With more than one reactor thread,
   * two threads can each receive this same entry while it is still ready.
   * Each thread gets it from its own concurrent epoll_wait call. That is the
   * classic thundering herd, and epoll itself does not prevent it. The code
   * holds this lock across the whole dispatch of this entry. That covers the
   * collection of read_reg and write_reg under the stripe lock below, and the
   * call of the callback itself. The stripe lock is different, because the
   * code holds it only for the collection. This lock is what guarantees that
   * the callback of one registration never runs at the same time as itself.
   * Both directions on one fd share this same entry. A read registration and
   * a write registration on the same fd therefore never run at the same time
   * either. That strict guarantee is deliberate. It serialises each direction
   * against itself and both directions against each other. A caller can
   * therefore put one shared resource across both directions of one fd. One
   * TLS connection object is such a resource. The caller needs no lock of its
   * own for that.
   *
   * This is NOT the same lock as the stripe lock. The stripe lock protects
   * the registry against concurrent ccol_event_loop_add, _remove and _modify
   * calls from ANY thread. That registry is the read_reg and write_reg slots
   * and the fd_index chmap. This lock protects the dispatch of one entry
   * against concurrent reactor threads. No thread holds this lock while it
   * tries to take the stripe lock of a different entry. No other code path
   * takes a stripe lock and then tries to take this lock. There is therefore
   * no new cycle in the lock order. */
  ccol_mutex_t dispatch_lock;

  /* The identity token that the caller sees through
   * ccol_event_loop_reg_generation. The code mints it once, from
   * loop->fd_generation_counter, when it creates the entry. It never mints a
   * new one when an existing entry gains its second direction. The code
   * copies this value into the reg->generation of every reg that attaches to
   * this entry. A read through a stale reg must use the copy of the reg.
   * It must never use owning_entry->generation. See the generation field of
   * ccol_event_reg for the reason. */
  uint64_t generation;

  /* A snapshot of loop->poller_batch_gen. The code takes it at the moment
   * when it pushes this entry onto loop->pending_entry_frees. See
   * _ccol_event_loop_defer_entry_free. The large comment above
   * _ccol_event_loop_reclaim_pending_frees describes the full reclaim scheme
   * that this field supports. It is 64 bits wide on every target, like
   * poller_batch_gen, so the defer_gen < poller_batch_gen test never meets a
   * wrap. */
  uint64_t defer_gen;

  /* True after ccol_event_loop_remove fully unregisters this entry, with no
   * direction and no reg left, and defers it for the free. The code sets it
   * exactly once, inside _ccol_event_loop_defer_entry_free, together with
   * defer_gen. _ccol_event_loop_rearm_entry_locked reads it. That function is
   * the shared helper that re-arms EPOLLONESHOT. It uses this flag to skip a
   * real fd. The application can already close or reuse that fd before a
   * dispatch job re-arms it. The generation counter guards callers against
   * the same hazard in other places. This flag applies that guard to the
   * internal re-arm call of this module. */
  _Atomic bool removed;

  /* This count is 1 higher while a dispatch job that references this entry
   * waits in the queue of a ctpool worker or runs on one. This happens only
   * when num_reactor_threads > 1. The path with num_reactor_threads == 1
   * never creates a job, so the count is always 0 there. A job needs the
   * entry to stay alive for its own duration. At the least it must lock and
   * unlock entry->dispatch_lock and re-arm the epoll interest after that. It
   * needs this whether or not ccol_event_loop_remove already fully
   * unregistered the entry in the meantime. The function
   * _ccol_event_loop_reclaim_pending_frees does not free a deferred entry
   * while this count is not zero, however far poller_batch_gen advanced. The
   * epoch check alone only proves that the batch that the POLLER already
   * fetched can no longer reference this entry. It says nothing about the job
   * of a ctpool worker that the code submitted well after the collection and
   * that still runs. */
  _Atomic size_t refcount;

  union {
    struct {
      ccol_event_reg_s *read_reg;
      ccol_event_reg_s *write_reg;
    } fd;
    /* For a queue or ccol_channel selectable: one reg for one entry, with
     * no sharing. */
    ccol_event_reg_s *reg;
  } as;

  /* For an fd selectable only. It says whether the interest set of
   * loop->epfd holds the fd of this entry now. The kernel reports EPOLLERR
   * and EPOLLHUP whatever the registered interest mask is, and even a mask of
   * 0 still gets them. A narrower mask alone can therefore never silence an
   * fd whose every live direction is paused with ccol_event_loop_pause.
   * Take a paused fd that is, or becomes, in an error or hangup state.
   * Without this flag, a level-triggered epoll_wait reports that fd again and
   * again. Each report shows nothing at all. The collection still fires and
   * raises the refcount. Only the callback is skipped, because
   * _ccol_event_loop_run_callback checks reg->paused again. That is an
   * unbounded CPU spin in the poller thread. With a dispatch_pool it is a
   * continuous stream of allocate, submit, dequeue, skip and re-arm cycles.
   * It also contradicts the documented contract of ccol_event_loop_pause,
   * which says that no callback fires, exactly as after a remove. A REMOVED
   * registration produces no further wake, because of EPOLL_CTL_DEL. A paused
   * registration with only a narrower mask cannot do that, because the mask
   * cannot opt out of the ERR and HUP monitoring.
   *
   * _ccol_event_loop_rearm_entry_locked can compute a mask with no real
   * interest bit left. This happens when every live direction is paused. The
   * code then removes the fd from the interest set of loop->epfd with
   * EPOLL_CTL_DEL. It adds the fd again once a direction resumes or a
   * new direction registers. It uses EPOLL_CTL_ADD and not EPOLL_CTL_MOD,
   * because MOD on an fd that is not registered now fails with ENOENT. This
   * flag is what lets _ccol_event_loop_add_fd and
   * _ccol_event_loop_rearm_entry_locked pick the correct epoll_ctl operation.
   * The code reads and writes it only while it holds the stripe lock of the
   * entry. This matches every other mutable field of this struct, so the flag
   * needs no atomic access. The flag is always false for a queue entry and
   * for a ccol_channel entry. The code adds the bridge eventfd of such an
   * entry once, in _ccol_event_loop_add_queue. This mechanism never removes
   * it and never adds it again. An eventfd has no such always-on error
   * condition, and a queue or ccol_channel registration cannot be paused at
   * all. */
  bool epoll_added;

  /* The code links this entry into loop->pending_entry_frees when it retires
   * the entry. See _ccol_event_loop_defer_entry_free below. Nothing uses an
   * entry again after the retire. This is why one field can safely carry both
   * the "live" state and the "pending free" state. */
  event_entry *pending_free_next;
};

/* One independent group of a mutex, an fd index and a queue-reg list. Exactly
 * one stripe handles a real fd for its whole lifetime. The same is true for
 * the private bridge eventfd of a queue or ccol_channel registration. See
 * _stripe_index_for_fd and the round-robin queue assignment in
 * ccol_event_loop_add. This is why no operation ever needs to hold the lock of
 * more than one stripe at once. */
typedef struct ccol_event_loop_stripe {
  ccol_mutex_t lock;
  chmap fd_index; /* int fd -> event_entry*, for real fds only */
  ccol_event_reg_s *queue_regs_head; /* the regs in this stripe that a queue or
                                      * a ccol_channel backs */
} ccol_event_loop_stripe_t;

/* A ccol_event_loop is an opaque value handle. Its top 32 bits are the slot
 * index, and its bottom 32 bits are the generation. See the doc comment on the
 * typedef in include/cthreadcomm.h. The code resolves that handle through this
 * table before it touches the struct ccol_event_loop_s* below it. This is what
 * lets __ccol_event_loop_destroy report two cases as a ccol_fatal_err, and not
 * as a use-after-free or a double free. The first case is a concurrent double
 * destroy, where two destroys race on the same live handle. The second case is
 * a sequential one, where a stale handle comes from an earlier destroy that
 * already finished. The code marks a slot as not in use the moment that it
 * releases the slot, and it raises the generation on every reuse. A stale
 * handle can therefore never name a later, unrelated loop that holds the same
 * slot index. This table matches chttpcli_slot_table and chttpsvr_slot_table.
 * See the copy of this comment in src/chttpclient.c for the full design
 * reason.
 *
 * The lock of this table is a read-write lock, not a plain mutex. The
 * function _ccol_event_loop_resolve only reads. It bounds-checks the index,
 * compares the generation and reads slot->ptr. It is the first step of
 * ccol_event_loop_pause, _resume, _modify, _remove and
 * ccol_event_loop_reg_generation. A caller like chttpserver pauses and resumes
 * the registration of a connection once for each request, so those calls run
 * at the full request rate. The only writers are
 * _ccol_event_loop_handle_slot_acquire and __ccol_event_loop_destroy. Each of
 * them runs once for the whole lifetime of a loop, not once for each request.
 * This matches the reg_slot_rwlock of this same file. See the comment of that
 * field on struct ccol_event_loop_s. It carries the same fork-safety subtlety:
 * a glibc write lock records the thread ID of the writer, and the child of a
 * fork() has a new one. See the doc comment of _cthreadcomm_atfork_prepare
 * for that hazard and for the way that the code handles it. */
typedef struct {
  struct ccol_event_loop_s *ptr; /* NULL when the slot is free */
  uint32_t generation;           /* minted fresh on every acquire. It rises for
                                     each slot index. It starts at 0 before the
                                     first use, and it becomes 1 on the first
                                     acquire. */
  bool in_use;
} ccol_event_loop_slot_t;

static struct {
  ccol_rw_lock_t rwlock;
  ccol_once_flag_t once;
  cvec slots;        /* A cvec of ccol_event_loop_slot_t. It grows only with
                         push_back, so an index is permanent after the allocation. */
  cvec free_indices; /* A cvec of uint32_t. A LIFO free list, O(1) reuse */
  /* How many slot indices the code could not push back onto free_indices,
     because that push could not allocate. Such a slot is fully released: its
     ptr is NULL and its in_use is false. But nothing names it. Without this
     counter it stays unreachable for the rest of the life of the process, and
     every later create grows the table by one more slot. An acquire that finds
     free_indices empty and this counter above zero recovers one slot with a
     scan of slots. That scan is unambiguous, because it only runs while the
     free list is empty: a released slot is either on that list or lost, and
     never both. The code reads and writes this counter only under the write
     lock. */
  size_t lost_indices;
  /* The code sets this when the process-exit destructor finds a loop that is
     still live and leaves this table alone. The destroy that frees the last
     slot after that then does the release that the destructor could not do.
     Without this flag, the code frees this table at exit in every case, while
     the destructor of another translation unit can still hold a loop. The
     destroy that it runs later then indexes a freed vector. */
  bool release_deferred;
} ccol_event_loop_slot_table = {0};

/* The definition is below, beside the process-exit destructor that sets the
   flag that this function reads. __ccol_event_loop_destroy calls it, and that
   function comes first in this file. */
static void _release_event_loop_slot_table_if_deferred_locked(void);

/* How many slots the table holds. The caller holds either side of the lock.
 * The vectors are NULL once the process-exit destructor released them, and
 * the table then holds no slot: a resolve finds nothing, and a create fails
 * cleanly instead of indexing a vector that is gone. */
static inline size_t _event_loop_slot_count_locked(void) {
  return ccol_event_loop_slot_table.slots
             ? cvector_elem_count(ccol_event_loop_slot_table.slots)
             : 0;
}

/* Forward declarations. The bodies are further below, after the declaration
 * of struct ccol_event_loop_s. They must come after it, because they
 * dereference the shutdown_lock, the reg_slot_rwlock and the stripes[] of a
 * live loop. _cthreadcomm_register_atfork_once registers them. See the doc
 * comment of queue_mutex_registry for the reason why this is ONE merged
 * ccol_at_fork() triple and not two independent ones. The one triple covers
 * the locks of ccol_event_loop_slot_table and the queue mutexes of
 * queue_mutex_registry. */
#if CCOL_FORK_SAFETY_REQUIRED
static void _cthreadcomm_atfork_prepare(void);
static void _cthreadcomm_atfork_release(void);
static void _cthreadcomm_atfork_child_release(void);
#endif

static void _ccol_event_loop_slot_table_init_globals(void) {
  if (ccol_rw_lock_init(ccol_event_loop_slot_table.rwlock) != 0)
    ccol_fatal_err("ccol_event_loop slot table: failed to initialize rwlock");
  ccol_event_loop_slot_table.slots =
      cvector_create(sizeof(ccol_event_loop_slot_t), NULL);
  if (!ccol_event_loop_slot_table.slots)
    ccol_fatal_err(
        "ccol_event_loop slot table: failed to allocate slots vector");
  ccol_event_loop_slot_table.free_indices =
      cvector_create(sizeof(uint32_t), NULL);
  if (!ccol_event_loop_slot_table.free_indices)
    ccol_fatal_err(
        "ccol_event_loop slot table: failed to allocate free-index vector");
}

/* Registers the ONE shared ccol_at_fork() triple. That triple covers the locks
 * of ccol_event_loop_slot_table and the queue mutexes of
 * queue_mutex_registry. See the doc comment of queue_mutex_registry for the
 * reason why this must be one merged registration and not two independent
 * ones. Two independent ones form an AB-BA deadlock cycle.
 *
 * This function first makes the plain data of BOTH structures ready on first
 * use. That data is a mutex and one or two cvecs, with no atfork wiring. Each
 * structure has its own ccol_call_once for that. The function does this
 * whichever structure triggered the call. A process can use only a
 * ccol_circular_queue or a ccol_dynamic_queue and never touch
 * ccol_event_loop. The mutex and the cvecs of ccol_event_loop_slot_table must
 * still be initialised there. This call registers the merged prepare and
 * release functions, and those functions always walk
 * ccol_event_loop_slot_table.
 *
 * fork() copies only the calling thread. See the doc comment of
 * _cthreadcomm_atfork_prepare for the full lock-inheritance hazard that this
 * closes for the parent and for the child. See the doc comment of
 * _cthreadcomm_atfork_child_release for two more hazards that only the child
 * has and that a plain shared release function cannot handle. The first one
 * is a join of a poller thread that exists only in the parent. The second one
 * is the live kernel epoll object that the child shares with the parent. */
static void _cthreadcomm_register_atfork_once(void) {
#if CCOL_FORK_SAFETY_REQUIRED
  /* Both ccol_call_once lines below stay inside this guard. The guard covers
   * more than the ccol_at_fork() registration alone. Every
   * ccol_event_loop-side caller of this function already makes
   * ccol_event_loop_slot_table ready on its own. Each one has its own
   * adjacent ccol_call_once(ccol_event_loop_slot_table.once, ...) call. See
   * _ccol_event_loop_resolve and _ccol_event_loop_handle_slot_acquire below.
   * The second line therefore matters only for a queue-only caller. Those
   * callers are _queue_mutex_registry_add and _queue_mutex_registry_remove.
   * They never touch ccol_event_loop_slot_table.once directly. They depend on
   * the merged atfork handlers below, and those handlers need the table
   * ready. When this macro is 0, no atfork machinery ever walks either
   * registry. Neither registry then needs to be ready on this path at all. */
  ccol_call_once(queue_mutex_registry.once, _queue_mutex_registry_init_globals);
  ccol_call_once(ccol_event_loop_slot_table.once,
                 _ccol_event_loop_slot_table_init_globals);
  ccol_at_fork(_cthreadcomm_atfork_prepare, _cthreadcomm_atfork_release,
               _cthreadcomm_atfork_child_release);
#endif
}

/* This function is not static on purpose. Another .c file of this library can
 * reach it, and chttpserver.c does. Such a caller must guarantee one thing.
 * The merged ccol_at_fork() triple of this module must register BEFORE the
 * triple of that caller. pthread_atfork runs the prepare handlers in LIFO
 * order. The prepare handler of the CALLER therefore runs FIRST at every
 * future fork(). It runs before _cthreadcomm_atfork_prepare, which is the
 * prepare handler of this module. That handler can then never lock
 * ccol_event_loop_slot_table.rwlock, or the locks of a live
 * ccol_event_loop, ahead of the caller.
 *
 * cthreadcomm.h does not declare this function. It is not part of the public
 * API. It is only a narrow, deliberate escape hatch. A caller must first read
 * this exact ordering requirement, and it must satisfy it. See the call site
 * in chttpserver.c for the full reasoning. That call site also describes the
 * lock-order inversion that this ordering makes impossible. ThreadSanitizer
 * reports that inversion when the two handler sets can register in either
 * order.
 *
 * A caller that never uses this function is not affected at all. The lazy,
 * ccol_call_once-guarded registration of this module still happens on its
 * own. It happens at the first use of a ccol_event_loop, a
 * ccol_circular_queue or a ccol_dynamic_queue. */
void _cthreadcomm_ensure_atfork_registered_before_caller(void) {
  ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
}

struct ccol_event_loop_s {
  int epfd;
  int shutdown_efd;

  /* The code registers this fd in epfd exactly like shutdown_efd. But this fd
   * is drained and re-armable. See the per-event loop of
   * _ccol_event_loop_thread_fn for the drain. shutdown_efd is different by
   * design. It is one-shot: nothing ever drains it and nothing looks at it
   * again. This fd exists for a ping on every future occasion where
   * poller_thread needs a prompt wake, and not only once.
   *
   * ccol_event_loop_remove pings this fd whenever the reg that it removes has
   * a non-NULL on_removed handler. The callback of that registration then
   * fires with a small, genuinely bounded latency. That bound is the next
   * between-batches point of poller_thread. The other outcome is a callback
   * that fires only when some unrelated fd becomes ready. Take a registration
   * that a caller removes on a loop that is otherwise idle. Nothing else is
   * registered there, and nothing else fires soon. Without this ping, the
   * on_removed notification and the memory reclaim that triggers it can
   * both wait. They wait for as long as the -1 (infinite) timeout of
   * epoll_wait keeps the poller blocked. That wait can last until the
   * destruction of the loop.
   *
   * That gap is not only a theoretical latency concern. Without this ping,
   * the on_removed_fires_for_an_ordinary_removal test of this module hangs
   * under ThreadSanitizer. A scheduling race sits between the thread creation
   * and the epoll_wait call. An unloaded, fast run passes that race
   * unnoticed. The slowdown of ThreadSanitizer widens it into a reliable
   * timeout.
   *
   * A ping through this fd is a latency optimization only. It is not the
   * correctness mechanism. The bounded epoll_wait retry of
   * EVENT_LOOP_RECLAIM_RETRY_MS is what guarantees a retry of a reclaim that
   * is still pending. See _ccol_event_loop_reclaim_pending_frees and
   * _ccol_event_loop_thread_fn. A ping from _ccol_event_reg_resolve_unpin can
   * rest on a stale snapshot of reg->removed against a concurrent
   * ccol_event_loop_remove. See the comment of that function. */
  int reclaim_wake_efd;

  /* Exactly one dedicated thread ever calls epoll_wait on epfd. This is true
   * for every configuration. It is what avoids the thundering-herd cost.
   * Several threads that share one epoll instance pay that cost. See the
   * comment of dispatch_pool for the mechanism that gives throughput instead.
   * num_reactor_threads == 1 also never creates dispatch_pool at all. That
   * one thread then polls and runs every callback inline. It also runs the
   * reclamation. See the epoch scheme below. */
  ccol_thread_id_t poller_thread;
  size_t num_reactor_threads; /* The caller-facing parameter. The total OS
                               * thread count for this loop is always exactly
                               * this value. It counts 1 for poller_thread.
                               * When the value is above 1, it also counts
                               * (num_reactor_threads - 1) worker threads that
                               * the ctpool owns. */

  /* NULL when num_reactor_threads == 1. In every other case it holds
   * num_reactor_threads - 1 worker threads. Those threads execute the
   * dispatch callbacks. poller_thread only collects readiness. It hands a
   * heap-allocated job to this pool with ctpool_submit. The queue of that
   * pool has no bound, so a submission never blocks the poller.
   * poller_thread never runs a callback itself. This design reuses the ctpool
   * of cthreadpool.c, which already has its own tests. A second worker pool
   * and queue inside this module would repeat that work. cthreadpool.c does
   * not depend on this header, so there is no risk of a circular dependency
   * in the other direction. */
  ctpool dispatch_pool;

  /* This lock guards only shutdown_started, joined and joined_cv. Those three
   * carry the one-shot leader and follower coordination of
   * ccol_event_loop_shutdown. The lock never touches per-fd state. Do not
   * confuse it with a per-stripe lock. The name is not "registry_lock" on
   * purpose. The registry itself lives in stripes[]. Each stripe has its own
   * lock, and this lock guards none of them. */
  ccol_mutex_t shutdown_lock;
  ccol_cond_var_t joined_cv;
  bool shutdown_started;
  bool joined;
  /* True once a __ccol_event_loop_destroy call owns the teardown of this
   * loop. That call sets it while the handle still resolves, so the
   * callbacks that its drain runs can still call back into this loop. A
   * second destroy call reads it and fails as fatal. The write side of
   * ccol_event_loop_slot_table.rwlock guards it, for both the read and the
   * write. */
  bool destroy_claimed;
  _Atomic bool shutting_down;

  /* The event_entry structs that ccol_event_loop_remove retired and that the
   * code did not free yet. See the comment of
   * _ccol_event_loop_defer_entry_free for the reason why a synchronous free
   * there is a use-after-free. This field is the head of a lock-free Treiber
   * stack. A push uses a CAS. A drain uses one atomic_exchange. The state is
   * loop-wide and not per-stripe, because entries from every stripe go onto
   * this one list. */
  _Atomic(event_entry *) pending_entry_frees;

  /* The ccol_event_reg structs whose refcount reached 0 and that the code did
   * not free yet. The common case is a refcount that reaches 0 at once, with
   * no dispatch in flight. A synchronous free there leaves a caller with a
   * ccol_event_reg* that points at freed memory. That caller can still pass
   * the pointer to ccol_event_loop_modify or to ccol_event_loop_remove. Both
   * functions promise a graceful ccol_invalid_args for a reg that is already
   * removed, and not undefined behaviour. See the comment of
   * _ccol_event_loop_defer_reg_free. This list is lock-free and has the same
   * shape as pending_entry_frees. */
  _Atomic(ccol_event_reg_s *) pending_reg_frees;

  /* A per-loop generation-tagged slot table for the PUBLIC ccol_event_reg
   * handle. That handle is a uint64_t value. Its top 32 bits are the slot
   * index, and its bottom 32 bits are the generation. The design mirrors the
   * process-wide design of ccol_event_loop_slot_table exactly. But this table
   * covers one loop and not the whole process. A reg already belongs to one
   * caller-supplied loop, so it needs no process-wide table. The handle
   * of a ccol_event_loop does need one, because it has no owning object to
   * scope it to.
   *
   * This table exists because a raw ccol_event_reg* in the hands of the
   * caller carries no liveness information. The caller cannot tell "still
   * live" from "already freed" without a dereference. That dereference is
   * itself unsafe once the object can already be gone.
   * ccol_event_loop_modify, _pause, _resume, _remove and
   * ccol_event_loop_reg_generation all need the stripe_idx of a reg at the
   * least. They need it before any lock-protected liveness check can run.
   *
   * The epoch scheme below (poller_batch_gen) proves only one thing. No STALE
   * POLLER BATCH can still reference a reg at the moment of its free. It says
   * nothing about a second application thread that dereferences that same
   * reg* through one of those five entry points. That thread can be genuinely
   * concurrent. It can also come later, on any schedule. A resolve of the
   * public handle through this table closes that gap completely and
   * unconditionally. See _ccol_event_reg_resolve and
   * _ccol_event_reg_resolve_unpin. A resolve of the handle of a
   * ccol_event_loop through ccol_event_loop_slot_table already gives the same
   * protection for a loop*. A stale or already-removed handle always fails a
   * mutex-protected check of the index and the generation. The code never
   * touches memory that can already be freed. ccol_event_loop_remove marks
   * the slot of a reg as not in use the moment that it removes that reg. It
   * also raises the generation of that slot. Every future resolve of that
   * handle value then fails at once. This holds whether or not the code
   * already freed the ccol_event_reg_s below it. See the comment of
   * _ccol_event_loop_reclaim_pending_frees for the moment of that free.
   *
   * The lock is a read-write lock and not a plain mutex.
   * _ccol_event_reg_resolve only reads. It bounds-checks the index, compares
   * the generation and reads slot->ptr. It runs on every
   * ccol_event_loop_pause, _resume and _modify call. A caller like
   * chttpserver pauses and resumes the registration of a connection once for
   * each request, so those calls run at the full request rate. The only
   * mutators are _ccol_event_reg_slot_acquire and
   * _ccol_event_reg_slot_release. Each of them runs once for the whole
   * lifetime of a registration, and not once for each request. They are
   * therefore rare. A plain mutex would serialise every concurrent resolve of
   * every connection in the process behind one lock. ccol_rw_lock_rdlock lets
   * concurrent resolves run together. It only excludes the rare acquire and
   * release mutations, and only those exclude it. Those mutations can
   * reallocate the backing storage of reg_slots. This brings one hazard:
   * the rwlock write lock of glibc records the thread ID of the writer, which
   * changes in the child of a fork(), and the plain mutexes of this module
   * record none. See the doc comment of
   * _cthreadcomm_atfork_prepare for that hazard and for the way that the code
   * handles it. */
  ccol_rw_lock_t reg_slot_rwlock;
  cvec reg_slots;        /* cvec of ccol_event_reg_slot_t; grows via push_back
                             only, indices permanent once allocated */
  cvec reg_free_indices; /* cvec of uint32_t; LIFO free list, O(1) reuse */
  /* How many slot indices the code could not push back onto
     reg_free_indices. Such a push fails when it cannot allocate. The index is
     still released: its slot carries ptr == NULL and in_use == false. But
     the free list does not reach it. The acquire path therefore scans for
     such a slot instead of a new growth of reg_slots. reg_slot_rwlock guards
     this counter, like the two vectors above. */
  size_t reg_lost_indices;

  /* The counter of the deferred-free reclamation. See the large comment above
   * _ccol_event_loop_reclaim_pending_frees for the full design. Exactly one
   * thread, poller_thread, ever calls epoll_wait, for every configuration.
   * "Safe to free" therefore needs one monotonic scalar and not a per-thread
   * array. poller_thread increments poller_batch_gen once at its own
   * between-batches point. That point is directly before the next epoll_wait
   * call. A deferred item carries its own defer_gen field. See event_entry
   * and ccol_event_reg. That field is a snapshot of poller_batch_gen from the
   * moment of the deferral. The EPOCH condition of the item is true once
   * defer_gen < poller_batch_gen. poller_thread then crossed a
   * between-batches point after the deferral. It therefore fully finished any
   * batch that it held in flight.
   *
   * For an event_entry this is only half of the condition. See the comment of
   * event_entry.refcount for the other half. That half exists because a job
   * of a ctpool worker can still use an entry well after poller_thread moves
   * on. This is true only when num_reactor_threads > 1. A ccol_event_reg has
   * no such second condition. Its own refcount fully covers the case where an
   * in-flight callback still needs that reg. This holds whether the callback
   * runs inline on poller_thread or on a dispatch_pool worker.
   *
   * The counter is 64 bits wide on every target. A 32-bit counter wraps after
   * 2^32 batches, which a busy loop reaches in days. An item deferred just
   * before the wrap then carries a defer_gen above every later value, never
   * becomes eligible, and keeps the poller on its bounded retry timeout for
   * the rest of the life of the loop. */
  _Atomic uint64_t poller_batch_gen;

  /* This counter mints the caller-visible identity token of
   * ccol_event_loop_reg_generation. It is loop-wide and monotonic, and it
   * starts at 1. The value 0 is reserved and means "no reg". See the doc
   * comment of ccol_event_loop_reg_generation. The code increments this
   * counter once for each NEW event_entry. It does not increment it for each
   * ccol_event_loop_add call. A second direction that joins an fd that is
   * already registered shares the generation of the existing entry. It mints
   * no new one. */
  _Atomic uint64_t fd_generation_counter;

  size_t max_events_per_wait;

  /* The epoll_wait buffer of poller_thread, max_events_per_wait entries.
   * ccol_event_loop_create_with_mprocs allocates it before it starts that
   * thread, and _ccol_event_loop_teardown_raw frees it after the join. The
   * thread itself allocates nothing, so a loop whose create returned a handle
   * always has a poller that dispatches. */
  ccol_poll_event *poller_events;

  /* Lock-striped fd/entry registry: num_stripes independent (mutex, chmap,
   * queue-reg list) triples. See ccol_event_loop_stripe_t and
   * _stripe_index_for_fd. */
  ccol_event_loop_stripe_t *stripes;
  size_t num_stripes;

  /* The round-robin cursor that assigns a queue or ccol_channel registration
   * to a stripe. See ccol_event_loop_add. A real fd is different. No second
   * call ever looks up the entry of a queue selectable, because the code
   * never combines two of them. That stripe assignment therefore has no
   * consistency rule to obey. Any rule that is deterministic for each
   * registration works. Round-robin is simpler than a hash of bridge_efd.
   * That fd does not even exist at the point where the code chooses the
   * stripe. The code creates it only after it takes the stripe lock.
   * Round-robin also gives a strictly better distribution. */
  _Atomic size_t next_queue_stripe;

  _Atomic size_t reg_count;

  ccol_memmgmt_procs_t *m_procs;

  /* _ccol_event_loop_resolve pins this count with a lock-free atomic
   * increment. The pin stays while a caller holds a struct
   * ccol_event_loop_s* that it resolved and did not yet release with
   * _ccol_event_loop_resolve_unpin. The field of the same name in chttpcli
   * and chttpsvr is different. Here the unpin side is ALSO a bare atomic
   * decrement, and not a lock-protected one. ccol_event_loop is lock-striped
   * for one reason: it keeps every hot per-registration call free of a single
   * global lock. Those calls are ccol_event_loop_add, _modify, _pause,
   * _resume and _remove. A lock-protected unpin would put exactly that lock
   * back on every one of them. __ccol_event_loop_destroy therefore polls
   * until this count reaches 0. See its own comment. A poll has no
   * lost-wakeup hazard, and a wait on a condition variable does. A poll never
   * depends on the delivery of a signal. */
  _Atomic size_t pending_resolve_count;

  /* The public handle value of this loop.
   * _ccol_event_loop_handle_slot_acquire mints it once, and nothing changes
   * it again. The callbacks need it. A ccol_event_readable_fn, a
   * ccol_event_writable_fn and a ccol_event_error_fn each take the public
   * ccol_event_loop handle as their own `loop` argument. They never take the
   * raw struct ccol_event_loop_s* that this file uses internally. Application
   * code inside a callback can call ccol_event_loop_modify, _pause, _resume,
   * _add or _remove back into the library. Such a call then goes through the
   * ordinary resolve and pin steps, like any other caller. See the two call
   * sites in _ccol_event_loop_run_callback. This is a plain field and needs
   * no synchronization. The code writes it exactly once, before the
   * constructor of this loop returns the handle to its caller. Dispatch can
   * start only once the caller has that handle back, because nothing can be
   * registered before that. No callback can therefore read this field before
   * it holds its final value. */
  ccol_event_loop self_handle;

  /* Only the CHILD-side fork handler of this process sets this flag to true.
   * That handler is _ccol_event_loop_atfork_child_release. It sets the flag
   * for every ccol_event_loop instance that is still marked in_use at the
   * moment of fork(). fork() duplicates only the calling thread. From the
   * point of view of this process, poller_thread is therefore only inert,
   * copy-on-write memory. No execution context for it ever existed here, and
   * none ever will.
   *
   * Once the flag is true, _ccol_event_loop_shutdown_internal must never do
   * two things. It must never call ccol_thread_join on poller_thread. That
   * join is undefined behaviour, because this process never created that
   * pthread_t and can never join it. It must also never write to
   * shutdown_efd. That eventfd is a real kernel object, and fork() shares it
   * instead of a copy. A write here would wrongly wake the poller thread
   * of the PARENT, which still genuinely uses that same object. The code must
   * treat both steps as already done.
   *
   * dispatch_pool needs no equivalent check here. It is a ctpool, and
   * cthreadpool.c carries its own identical foreign_since_fork field. That
   * field already makes an unconditional ctpool_shutdown_drain call safe,
   * whatever this flag holds. Without that field on the ctpool side, one
   * sequence crashes. Fork a process that holds a live ccol_event_loop with
   * num_reactor_threads > 1. Then call ccol_event_loop_destroy on the
   * inherited handle in the child. That SIGSEGVs inside the
   * __pthread_clockjoin_ex of glibc, which the worker-thread join loop of
   * ctpool_shutdown_drain reaches.
   *
   * This flag is never true for a loop that this process really created with
   * ccol_event_loop_create_with_mprocs. The compiler removes the flag
   * completely when CCOL_FORK_SAFETY_REQUIRED is 0. See the doc comment of
   * that macro in common.h. Every site that would read this field then takes
   * the same path that it already takes when the field is false. */
#if CCOL_FORK_SAFETY_REQUIRED
  _Atomic bool foreign_since_fork;
#endif

#ifdef RUNNING_UNIT_TESTS
  /* Test-only. The code increments it once for each epoll_wait call that
   * completes on poller_thread. It does this whatever number of events that
   * call returned, and also for zero events. A test can then detect a
   * busy-spin directly: this counter then races ahead by a large amount
   * inside a short, bounded sampling window. The test needs no flaky
   * measurement of the wall clock or of the CPU usage. See
   * ccol_event_loop_poller_iterations_for_tests. The field stays behind
   * RUNNING_UNIT_TESTS, so a production build pays nothing for it. This obeys
   * the performance-first policy of this project. */
  _Atomic uint64_t poller_iterations_for_tests;
  /* Test-only. The count of event entries that the epoch reclaim of
   * _ccol_event_loop_reclaim_pending_frees freed. A test reads it to know
   * that the entry of a removed registration is really gone before it
   * checks a later registration of the same fd. See
   * _ccol_event_loop_entries_reclaimed_for_tests. */
  _Atomic uint64_t entries_reclaimed_for_tests;
#endif
};

/* ========================================================================== */
/*                         FORK SAFETY (pthread_atfork)                       */
/* ========================================================================== */

#if CCOL_FORK_SAFETY_REQUIRED
/* fork() duplicates only the calling thread. Some OTHER thread can hold a
 * lock at that instant. The child then inherits that lock in a permanently
 * locked state, because no thread survives in the child that can unlock it.
 * This function therefore locks every lock that this module can plausibly
 * hold at an arbitrary instant. There are three groups of them. The first
 * is ccol_event_loop_slot_table.rwlock, which covers the whole process. The
 * second is the shutdown_lock, reg_slot_rwlock and stripes[].lock of
 * each still-live loop. The third is the mutex of every live
 * ccol_circular_queue and ccol_dynamic_queue.
 * Every ccol_event_loop_create, _destroy, _add, _remove, _modify, _pause and
 * _resume call takes the first one. It takes it through
 * _ccol_event_loop_resolve or _ccol_event_loop_handle_slot_acquire. The queue
 * mutexes come from queue_mutex_registry. See the doc comment of that
 * registry above.
 *
 * prepare() takes all of it before fork() can go on. fork() therefore
 * completes only when no thread holds one of them for a moment. parent() and
 * child() release all of it again through one shared function. Every PLAIN
 * mutex in this module uses the default "normal" pthread mutex type. That
 * type does no owner or TID tracking on Linux glibc. A plain
 * pthread_mutex_unlock is therefore well defined even from a thread that did
 * not lock it. For anything that the forking thread did not hold itself, the
 * original thread does not exist in the child at all.
 *
 * ccol_event_loop_slot_table.rwlock and reg_slot_rwlock are the two
 * exceptions. Both need a reinit on the child side. prepare() above takes the
 * write side of both on the thread that calls fork(), once every other holder
 * has released it: ccol_event_loop_slot_table.rwlock at its outer scope, and
 * reg_slot_rwlock inside the per-loop walk of Phase 1. The unlock of a glibc
 * rwlock tells a writer unlock from a reader unlock by comparing the writer
 * TID that it recorded with the TID of the caller, and fork() gives the one
 * thread of the child a new TID. A plain ccol_rw_lock_unlock in the child
 * therefore returns 0 and releases nothing, and every later lock of it there
 * blocks for ever. The atfork handling of clog_slot_table.rwlock in
 * clogger.c, which this code mirrors, handles the same case. A plain mutex
 * keeps no TID, which is why only these two rwlocks, among every lock here,
 * need the reinit.
 *
 * Without this locking, the hang is real and reproducible, not theoretical.
 * Take a thread that continuously creates and destroys unrelated
 * ccol_event_loop instances, raced against repeated fork() calls. Roughly 1
 * forked child in 1000 then hangs permanently. It hangs the moment that it
 * tries its own brand-new ccol_event_loop_create_with_mprocs call, because it
 * inherited the lock of ccol_event_loop_slot_table in a locked state. Now
 * take one continuously busy, multi-threaded reactor. It mirrors the
 * long-lived, process-wide reactors of chttpserver.c and chttpclient.c. Fork
 * it while it is busy. Roughly half of all forked children then hang. They
 * hang the moment that they try one more ccol_event_loop_add on the loop that
 * they inherited. The lock of a stripe arrives locked. That stripe
 * happens to hold the fd that the vanished poller and dispatch threads were
 * still working on.
 *
 * This function deliberately does NOT extend to an individual
 * event_entry.dispatch_lock. There is only one safe way to discover every
 * live entry. That way is a walk of the fd_index and queue_regs_head of
 * a stripe, under the lock of that exact stripe. This file uses only one
 * ordering between the dispatch lock of an entry and a stripe lock. A lock
 * of that dispatch lock while the stripe lock is held is the exact reverse
 * of it. See the field comment of event_entry.dispatch_lock. The code always
 * takes dispatch_lock first. It releases that lock before it considers a
 * stripe lock, and never the other way around. A lock here would therefore
 * add a genuine new ABBA deadlock. The other side is an ordinary reactor
 * thread inside _ccol_event_loop_handle_event, which holds dispatch_lock and
 * wants a stripe lock. That fixes nothing.
 *
 * A forked child never has a reactor thread or a dispatch thread of its own
 * for a loop that it merely inherited. The reason is that fork() duplicates
 * only the calling thread. Nothing in the child can therefore ever
 * legitimately dispatch through that loop again, whatever this gap is. The
 * gap leaves one thing unprotected. That thing is a later
 * ccol_event_loop_destroy() of that exact inherited loop, in the child,
 * that races a dispatch_lock. Some other
 * parent-side thread held that lock at fork time, and that thread is gone by
 * then. POSIX already makes a ccol_mutex_destroy on a still-locked mutex
 * undefined behaviour, and that is true with no fork at all. This case is
 * narrower and far less likely than the two hazards above, which happen at
 * roughly 1-in-1000 and 1-in-2 rates. The comment documents it rather than
 * closing it. A safe closure needs a different way to discover live entries.
 * A dedicated live-entry list with its own lock is one such way, like the
 * live_shareds of clogger.c. That is a materially larger change than the
 * scope of this mechanism.
 *
 * This function DOES extend to the wait_mtx of every ccol_event_reg_s
 * that a queue or a ccol_channel backs. It also extends to the cq->mutex or
 * dq->mutex of the queue below that reg. dispatch_lock above is different.
 * The code must lock the two in exactly this relative order. That order
 * matches every real nested-locking pattern in this file:
 *
 *   1. The shutdown_lock and reg_slot_rwlock (write side) of every live
 *      loop, and the lock of every stripe. This matches the
 *      stripe->lock before cq->mutex nesting of _ccol_event_loop_add_queue
 *      and of _ccol_event_loop_remove_unlink. A caller calls both of them
 *      with the lock of the owning stripe already held. That lock must
 *      therefore already be held here before the code touches the underlying
 *      queue mutex of any queue-backed registration of that stripe.
 *   2. The mutex of every live queue, from queue_mutex_registry. The code
 *      locks each one exactly once. This is true whatever number of
 *      ccol_event_loop registrations reference it, across one or more stripes
 *      and loops, or none at all. The registry never lists an address twice.
 *      One pass over it, as this self-contained phase, can therefore never
 *      double-lock anything.
 *   3. The wait_mtx of every queue-backed registration. This matches the
 *      cq->mutex before wait_mtx nesting of _notify_waiter. Any ordinary
 *      producer or consumer thread calls _notify_waiter from a plain send or
 *      receive on a queue that this registration watches. That thread is
 *      fully independent of the reactor and dispatch threads of this
 *      loop, and it always holds the mutex of that queue.
 *
 * A SEPARATE, independent ccol_at_fork() registration for the queue mutexes
 * is a genuine deadlock, and not only a theoretical concern. Create a
 * ccol_circular_queue before the first ccol_event_loop of the process. That
 * registers the separate ccol_at_fork() triple first. pthread_atfork runs the
 * prepare handlers in REVERSE registration order. The prepare of
 * ccol_event_loop therefore runs FIRST and locks the wait_mtx of a
 * queue-backed registration. The separate prepare of the queue registry runs
 * SECOND. It tries to lock the cq->mutex of that SAME queue. A genuinely
 * concurrent ccol_circq_send_zc call already holds that mutex, and it is
 * itself blocked inside _notify_waiter and wants that same wait_mtx. That is
 * a textbook AB-BA cycle, with the forking thread deadlocked inside fork()
 * itself.
 *
 * The three-phase design above closes this completely by construction. There
 * is exactly ONE ccol_at_fork() registration. No accident of "which of two
 * independent handler sets runs first" is left to depend on. Its own three
 * phases meet constraint 1 and constraint 3 above at the same time, for every
 * queue-backed registration. This holds whichever stripe and loop that
 * registration belongs to. It also holds when its queue has no
 * ccol_event_loop registration at all.
 *
 * This function only ever walks a slot with in_use == true. That mirrors the
 * exact condition that _ccol_event_loop_resolve already trusts. That
 * condition is the sole sign that slot->ptr is safe to dereference. The
 * function __ccol_event_loop_destroy keeps in_use set while its graceful
 * shutdown joins the poller thread and drains dispatch_pool, so this walk
 * still covers a loop in that phase. It clears in_use under the write side of
 * this same ccol_event_loop_slot_table.rwlock once that shutdown returns, and
 * only then frees every entry, every registration and the loop struct. No
 * thread of the loop runs by then, so the locks that this walk skips have no
 * holder left but the destroying thread itself. */
static void _cthreadcomm_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(ccol_atfork_module_cthreadcomm);
#endif
  ccol_mutex_lock(queue_mutex_registry.mutex);
  ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);

  size_t n_loops = _event_loop_slot_count_locked();

  /* Phase 1: the shutdown_lock, reg_slot_rwlock and stripe locks of every
   * live loop. This phase deliberately does NOT touch wait_mtx or the
   * mutex of any queue yet. See the doc comment of this function for the
   * reason why those must wait for phase 2 and phase 3.
   *
   * reg_slot_rwlock takes its WRITE side here, and not its read side. fork()
   * must see this table as fully quiesced. No resolve, acquire or release may
   * be in flight anywhere. Only the write side guarantees that against every
   * other locker, readers included. See the comment of that field for the
   * reason why it is a rwlock at all. See the comment of
   * _cthreadcomm_atfork_release_impl for the child-side release hazard that
   * follows. This specific lock is subject to that hazard, and no other lock
   * that this function takes is. */
  for (size_t i = 0; i < n_loops; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct ccol_event_loop_s *loop = slot->ptr;
    ccol_mutex_lock(loop->shutdown_lock);
    ccol_rw_lock_wrlock(loop->reg_slot_rwlock);
    for (size_t s = 0; s < loop->num_stripes; s++) {
      ccol_mutex_lock(loop->stripes[s].lock);
    }
  }

  /* Phase 2: every live queue's own mutex, exactly once each, now that
   * every stripe lock (phase 1) is already held. */
  size_t n_addrs = queue_mutex_registry.addrs
                       ? cvector_elem_count(queue_mutex_registry.addrs)
                       : 0;
  for (size_t k = 0; k < n_addrs; k++) {
    ccol_mutex_t *m = ((_queue_mutex_registry_entry *)cvector_at(
                           queue_mutex_registry.addrs, k))
                          ->mutex;
    ccol_mutex_lock(*m);
  }

  /* Phase 3: every queue-backed registration's own wait_mtx, now that
   * every queue's own mutex (phase 2) is already held. */
  for (size_t i = 0; i < n_loops; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct ccol_event_loop_s *loop = slot->ptr;
    for (size_t s = 0; s < loop->num_stripes; s++) {
      for (ccol_event_reg_s *reg = loop->stripes[s].queue_regs_head; reg;
           reg = reg->loop_list_next) {
        ccol_mutex_lock(reg->wait_mtx);
      }
    }
  }
}

/* Both parent() and child() share this function. See the doc comment of
 * _cthreadcomm_atfork_prepare for two things. It says why a plain unlock, and
 * not a reinit, is correct in both branches for the mutexes of this module.
 * It also describes the three-phase design that this function mirrors. This
 * function runs the phases in reverse order. That order does not matter for
 * correctness with the plain, non-recursive mutexes of this module, because
 * an unlock never blocks and can never deadlock.
 *
 * It is safe to walk the identical structure that prepare() just walked and
 * to release every lock. Nothing can have changed the slot table, the queue
 * registry, the stripe count of a live loop or its registration list in
 * between. Every lock that such a change needs is still held at this exact
 * point.
 *
 * With is_child, the code does more work for every still-live loop:
 *  - It marks the loop foreign_since_fork. See the comment of that field.
 *    A later ccol_event_loop_shutdown or _destroy call in THIS process then
 *    never joins poller_thread and never signals shutdown_efd.
 *  - It resets the pending_resolve_count of the loop to 0. The is_child
 *    branch of _ctpool_atfork_release_impl does this for the identical
 *    reason. A parent-side thread that is now gone can have left the count
 *    permanently above zero, from the point of view of this process. See the
 *    comment of that function for the full reasoning. It applies here
 *    unchanged.
 *  - It replaces the local epfd of this process with a brand new, empty
 *    epoll instance. loop->epfd is a real kernel object, and so is the
 *    registration of loop->shutdown_efd inside it. fork() duplicates that
 *    object and does not deep-copy it. The fd number of this process
 *    still names the SAME underlying epoll instance. The poller thread of
 *    the still-running parent goes on to call epoll_wait on that instance.
 *
 *    Without the replacement, every future epoll_ctl call of this process
 *    silently changes the live interest set of the PARENT. Those calls come
 *    from ccol_event_loop_add, _remove, _modify, _pause and _resume. All of
 *    them are otherwise still fully working bookkeeping operations on the
 *    private, copy-on-write registry of this loop. Take an ADD for a
 *    genuinely new fd that this process registers. It can hand the poller
 *    thread of the parent an epoll_event whose ev.data.ptr is an
 *    event_entry*. That pointer only makes sense in the already-diverged
 *    heap of this process. Take a DEL for an inherited registration. It rips
 *    the still-wanted interest of the parent in that fd out from under
 *    it.
 *
 *    A swap of the epfd of this process decouples every future epoll_ctl
 *    call of this process from the kernel object of the parent. It costs the
 *    correctness of this process nothing. No poller thread of its own
 *    survives the fork, so this process was never going to call epoll_wait on
 *    that instance for a real dispatch. See the doc comment of
 *    ccol_event_loop_add: nothing in the child can ever legitimately dispatch
 *    through that loop again. A later ccol_event_loop_remove or
 *    ccol_event_loop_destroy of an inherited, pre-fork registration then
 *    finds nothing to EPOLL_CTL_DEL on this new, empty instance. The code
 *    tolerates that silently, because this whole file already ignores the
 *    return value of every such epoll_ctl call. That is exactly the point. It
 *    must not find, and must not touch, the still-live registration of
 *    the parent for that same fd.
 *
 *    The code also tolerates a failure to create the replacement instance
 *    silently. In that rare case it leaves loop->epfd as the original, still
 *    shared one. It does not abort a fork() that otherwise succeeded. Nothing
 *    else in this function depends on that success. */
static void _cthreadcomm_atfork_release_impl(bool is_child) {
  size_t n_loops = _event_loop_slot_count_locked();

  for (size_t i = 0; i < n_loops; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct ccol_event_loop_s *loop = slot->ptr;

    if (is_child) {
      atomic_store(&loop->foreign_since_fork, true);
      atomic_store(&loop->pending_resolve_count, (size_t)0);

      int old_epfd = loop->epfd;
      int new_epfd = ccol_poll_create();
      if (new_epfd >= 0) {
        loop->epfd = new_epfd;
        close(old_epfd);
      }
    }

    for (size_t s = 0; s < loop->num_stripes; s++) {
      for (ccol_event_reg_s *reg = loop->stripes[s].queue_regs_head; reg;
           reg = reg->loop_list_next) {
        ccol_mutex_unlock(reg->wait_mtx);
      }
    }
  }

  /* The mutex of every live queue, exactly once each. This one
   * self-contained pass unlocks them, and it mirrors phase 2 of prepare()
   * exactly. The code does not interleave it into the loop above.
   * Registrations across more than one stripe or loop can share the mutex of
   * one queue. More than one unlock of it is undefined behaviour for the
   * plain mutexes of this module. */
  size_t n_addrs = queue_mutex_registry.addrs
                       ? cvector_elem_count(queue_mutex_registry.addrs)
                       : 0;
  for (size_t k = 0; k < n_addrs; k++) {
    ccol_mutex_t *m = ((_queue_mutex_registry_entry *)cvector_at(
                           queue_mutex_registry.addrs, k))
                          ->mutex;
    ccol_mutex_unlock(*m);
  }

  for (size_t i = 0; i < n_loops; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct ccol_event_loop_s *loop = slot->ptr;
    for (size_t s = 0; s < loop->num_stripes; s++) {
      ccol_mutex_unlock(loop->stripes[s].lock);
    }
    /* Phase 1 above took the write side of reg_slot_rwlock on this same
     * thread, the one that calls fork(); pthread_atfork runs prepare(),
     * parent() and child() on it. In the parent a plain ccol_rw_lock_unlock
     * from HERE releases it, because the unlock of a glibc rwlock releases
     * the write side only when the caller's TID matches the writer TID that
     * it recorded, and here it does.
     *
     * In the child it does not match: fork() gives the one thread of the
     * child a new TID. A plain unlock therefore returns 0 and releases
     * nothing, and every later resolve in this child blocks for ever. The
     * child reinitializes the rwlock instead, as _clog_atfork_release in
     * clogger.c does for clog_slot_table.rwlock. That is safe because the
     * child has exactly one thread at this point, so nothing can be waiting
     * on the lock. */
    if (is_child) {
      if (ccol_rw_lock_init(loop->reg_slot_rwlock) != 0)
        ccol_fatal_err(
            "ccol_event_loop atfork release: failed to reinit reg_slot_rwlock");
    } else {
      ccol_rw_lock_unlock(loop->reg_slot_rwlock);
    }
    ccol_mutex_unlock(loop->shutdown_lock);
  }

  /* prepare() above took the write side of ccol_event_loop_slot_table.rwlock
   * on this same thread. See the identical in_child and else treatment of
   * reg_slot_rwlock directly above for why the child reinitializes it.
   * src/cthreadpool.c carries its own copy of that exact treatment in
   * _ctpool_atfork_release_impl. This is the copy of the identical hazard for
   * the process-wide loop table, and not a new one. */
  if (is_child) {
    if (ccol_rw_lock_init(ccol_event_loop_slot_table.rwlock) != 0)
      ccol_fatal_err(
          "ccol_event_loop atfork release: failed to reinit "
          "ccol_event_loop_slot_table "
          "rwlock");
  } else {
    ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  }
  ccol_mutex_unlock(queue_mutex_registry.mutex);
}

static void _cthreadcomm_atfork_release(void) {
  _cthreadcomm_atfork_release_impl(false);
}

/* The child-side counterpart of _cthreadcomm_atfork_release. See the
 * comment of _cthreadcomm_atfork_release_impl for the extra, child-only work
 * and for the reason why the code needs each part of it. This function must
 * run first. Application code in this process can reach the shutdown,
 * destroy, add, remove or modify path of one of these loops. This function
 * must run before any of that. The child
 * handler of pthread_atfork runs synchronously, as part of the return of
 * fork() itself. It runs strictly before the return value of fork() reaches
 * the calling code. */
static void _cthreadcomm_atfork_child_release(void) {
  _cthreadcomm_atfork_release_impl(true);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                    EVENT_LOOP HANDLE RESOLVE / UNPIN                       */
/* ========================================================================== */

/* Resolves h and pins the result against a concurrent destroy. It returns
 * NULL in three cases. The first is an h of 0. The second is garbage. The
 * third is a slot that is free now, or that an earlier acquire already
 * reused, which gives the wrong generation. On success the caller MUST call
 * _ccol_event_loop_resolve_unpin(result) exactly once. It must make that call
 * as soon as it stops touching the resolved struct ccol_event_loop_s*. */
static struct ccol_event_loop_s *_ccol_event_loop_resolve(ccol_event_loop h) {
  ccol_call_once(ccol_event_loop_slot_table.once,
                 _ccol_event_loop_slot_table_init_globals);
  ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol_rw_lock_rdlock(ccol_event_loop_slot_table.rwlock);
  struct ccol_event_loop_s *raw = NULL;
  if (idx < _event_loop_slot_count_locked()) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* The code takes no lock on raw itself here. This matches the field
   * comment of this loop on pending_resolve_count. The lock striping of
   * ccol_event_loop exists to keep every hot per-registration call free of a
   * single global lock. This resolve step must not add one. It is safe,
   * because raw is guaranteed to be allocated at this point. Only one step
   * can make raw unsafe to touch: the slot-release step of
   * __ccol_event_loop_destroy. That step needs the write side of
   * ccol_event_loop_slot_table.rwlock. It can therefore never run at the same
   * time as this read side. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  return raw;
}

static void _ccol_event_loop_resolve_unpin(struct ccol_event_loop_s *raw) {
  /* A bare atomic decrement, with no lock and no broadcast. chttpcli and
   * chttpsvr use a lock-protected decrement instead. See the field
   * comment of pending_resolve_count for the reason why this difference is
   * correct here. __ccol_event_loop_destroy polls until this count reaches 0.
   * It does not sleep on a condition variable. There is therefore no
   * lost-wakeup hazard to guard against, and nothing to broadcast to. */
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
}

/* Allocates a fresh slot for loop, or reuses a freed one. It returns the
 * handle that results, or 0 when it cannot allocate.
 * ccol_event_loop_create_with_mprocs calls it once. It calls it after the
 * loop is otherwise fully built. That build includes the poller thread of the
 * loop and, when the caller configured one, its dispatch_pool. A failed slot
 * acquire at this point must stop them, and not only free memory. See the
 * comment of that function for the reason. */
#ifdef RUNNING_UNIT_TESTS
/* Test-only. It makes the next push onto the loop free list behave exactly
 * like an allocation failure. It then disarms itself. Without it, the
 * lost-index recovery below needs a real out-of-memory condition at one
 * specific cvector_push_back call. An ordinary test run cannot reach that. */
static _Atomic bool g_fail_next_loop_free_index_push = false;

void _ccol_event_loop_force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_fail_next_loop_free_index_push, true);
}
#endif

/* Hands idx back for reuse. The caller holds the write lock.
 *
 * The code records a failed push and does not swallow it. The slot that the
 * index names is already fully released. A silent drop of it strands that
 * index for the life of the process. Every later create then grows the table
 * again. See ccol_event_loop_slot_table.lost_indices. */
static void _ccol_event_loop_free_index_release_locked(uint32_t idx) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_fail_next_loop_free_index_push, false)) {
    ccol_event_loop_slot_table.lost_indices++;
    return;
  }
#endif
  if (cvector_push_back(ccol_event_loop_slot_table.free_indices, &idx) !=
      ccol_success)
    ccol_event_loop_slot_table.lost_indices++;
}

/* Recovers one slot index that a failed free-list push stranded. See
 * ccol_event_loop_slot_table.lost_indices. It scans for a slot that is
 * released and that nothing names. The caller holds the write lock and
 * already found free_indices empty. That is what makes "ptr is NULL and
 * in_use is false" mean lost and not merely free. A slot that the code
 * released with success is on that list. A slot whose destroy is still in
 * progress keeps a non-NULL ptr until the very end of that destroy. The
 * destroy clears in_use once its shutdown has drained, and it clears ptr only
 * in its final locked step. The two conditions together therefore skip exactly
 * that window. This function only ever returns an index that the table already
 * holds. It can therefore never take the table past any slot ceiling.
 *
 * The function is out of line, and the code reaches it only when the counter
 * is above zero. An ordinary acquire therefore pays one comparison for it.
 * The function zeroes the counter when the scan finds nothing. A counter that
 * somehow outlives its slot can therefore not make every future acquire scan
 * the table. */
static __attribute__((noinline)) bool
_ccol_event_loop_reclaim_lost_index_locked(uint32_t *out_idx) {
  size_t slot_count = cvector_elem_count(ccol_event_loop_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      ccol_event_loop_slot_table.lost_indices--;
      return true;
    }
  }
  ccol_event_loop_slot_table.lost_indices = 0;
  return false;
}

static ccol_event_loop _ccol_event_loop_handle_slot_acquire(
    struct ccol_event_loop_s *loop) {
  ccol_call_once(ccol_event_loop_slot_table.once,
                 _ccol_event_loop_slot_table_init_globals);
  ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  /* The process-exit destructor released the table. No handle can be minted
   * after that, and the create fails like an allocation failure. */
  if (!ccol_event_loop_slot_table.slots) {
    ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
    return 0;
  }
  uint32_t idx;
  ccol_event_loop_slot_t *slot;
  if (cvector_elem_count(ccol_event_loop_slot_table.free_indices) > 0) {
    cvector_pop_back(ccol_event_loop_slot_table.free_indices, &idx);
    slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
  } else if (ccol_event_loop_slot_table.lost_indices > 0 &&
             _ccol_event_loop_reclaim_lost_index_locked(&idx)) {
    slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
  } else {
    ccol_event_loop_slot_t fresh = {0};
    if (cvector_push_back(ccol_event_loop_slot_table.slots, &fresh) !=
        ccol_success) {
      ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(ccol_event_loop_slot_table.slots) - 1;
    slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
  }
  slot->generation++;
  /* This skips the one generation value that collides with the reserved
   * "invalid handle" sentinel, which is 0. That collision comes after about
   * 2^32 reuses of this exact slot index. See the identical guard in
   * chttpcli_handle_slot_acquire for the full reasoning. */
  if (slot->generation == 0) slot->generation++;
  slot->ptr = loop;
  slot->in_use = true;
  ccol_event_loop h =
      ((ccol_event_loop)idx << 32) | (ccol_event_loop)slot->generation;
  ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  return h;
}

/* A multiplicative hash, reduced modulo num_stripes. It uses the constant of
 * Knuth. An fd is chosen by the kernel and not by a peer, so an unkeyed
 * hash is enough here. The code avoids a plain
 * fd % num_stripes on purpose. An fd is a small, kernel-sequential integer,
 * and a plain modulo risks clustering. For example, a pattern of only even
 * fds lands in half of the stripes when num_stripes is a power of two. This
 * function covers fd selectables only. A queue selectable and a ccol_channel
 * selectable get their stripe from loop->next_queue_stripe instead. See
 * ccol_event_loop_add. The stripe of such a selectable has no consistency
 * rule to obey at all. */
static size_t _stripe_index_for_fd(struct ccol_event_loop_s *loop, int fd) {
  uint32_t h = (uint32_t)fd * 2654435761u;
  return (size_t)h % loop->num_stripes;
}

/* fd_index maps a ccol_int key to a ccol_pointer value. Both are integral and
 * 8 bytes or fewer. The should_use_open_addressing() of chashmap
 * therefore sends this map to the open-addressing backend, and not to
 * separate chaining. The oa_slot storage of that backend is
 * _Alignas(max_align_t). The memcpy below is defense-in-depth and not a live
 * alignment requirement. The code keeps it for consistency with the
 * chmap-backed pointer storage of cjson, cyaml, clrucache and chttpclient.
 * Those four use separate chaining, where the packed chmap_entry SSO union
 * genuinely needs the memcpy. A caller of this map must also not assume one
 * particular backend for ever.
 *
 * These functions all work on the fd_index and queue_regs_head of ONE
 * stripe. The caller passes that stripe in directly. That caller already
 * computed the right stripe, with _stripe_index_for_fd or with the
 * round-robin counter, and it already locked the stripe. These functions
 * never work on the loop as a whole. */
static event_entry *_fd_registry_find(ccol_event_loop_stripe_t *stripe,
                                      int fd) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  const cmap_pair *val_pair = NULL;
  if (chmap_get_elem_ref(stripe->fd_index, &key_pair, &val_pair) !=
      ccol_success) {
    return NULL;
  }
  event_entry *entry;
  memcpy(&entry, val_pair->ptr, sizeof(entry));
  return entry;
}

/* Inserts fd->entry. The caller must already have confirmed that fd is not
 * present. An earlier _fd_registry_find call that returned NULL confirms
 * that. */
static bool _fd_registry_insert(ccol_event_loop_stripe_t *stripe, int fd,
                                event_entry *entry) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  cmap_pair val_pair = {.ptr = &entry, .size = sizeof(entry)};
  return chmap_insert_elem(stripe->fd_index, &key_pair, &val_pair) ==
         ccol_success;
}

static void _fd_registry_remove(ccol_event_loop_stripe_t *stripe, int fd) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  chmap_delete_elem(stripe->fd_index, &key_pair);
}

static void _loop_queue_list_add(ccol_event_loop_stripe_t *stripe,
                                 ccol_event_reg_s *reg) {
  reg->loop_list_prev = NULL;
  reg->loop_list_next = stripe->queue_regs_head;
  if (stripe->queue_regs_head) stripe->queue_regs_head->loop_list_prev = reg;
  stripe->queue_regs_head = reg;
}

static void _loop_queue_list_remove(ccol_event_loop_stripe_t *stripe,
                                    ccol_event_reg_s *reg) {
  if (reg->loop_list_prev)
    reg->loop_list_prev->loop_list_next = reg->loop_list_next;
  else
    stripe->queue_regs_head = reg->loop_list_next;
  if (reg->loop_list_next)
    reg->loop_list_next->loop_list_prev = reg->loop_list_prev;
}

/* Resolves the queue mutex for sel. It also resolves the correct
 * sel_{read,write}_waiters_head and sel_{read,write}_rotor pointers. sel is a
 * circq or a dynq only. An fd selectable never reaches here.
 * ccol_selectable_from_chan already resolved a chan selectable down to a
 * concrete circq before sel reaches ccol_event_loop_add. */
static void _queue_sel_locate(ccol_selectable *sel, ccol_mutex_t **out_mtx,
                              ccol_sel_waiter ***out_head,
                              ccol_sel_waiter ***out_rotor) {
  if (sel->type == ccol_selectable_circq) {
    ccol_circular_queue *cq = sel->cq;
    *out_mtx = &cq->mutex;
    *out_head = (sel->dir == ccol_select_read) ? &cq->sel_read_waiters_head
                                               : &cq->sel_write_waiters_head;
    *out_rotor = (sel->dir == ccol_select_read) ? &cq->sel_read_rotor
                                                : &cq->sel_write_rotor;
  } else {
    ccol_dynamic_queue *dq = sel->dq;
    *out_mtx = &dq->mutex;
    *out_head = (sel->dir == ccol_select_read) ? &dq->sel_read_waiters_head
                                               : &dq->sel_write_waiters_head;
    *out_rotor = (sel->dir == ccol_select_read) ? &dq->sel_read_rotor
                                                : &dq->sel_write_rotor;
  }
}

/* These two give the queue that a resolved queue selectable names, and the
 * dispatch-reference counter of that queue. sel is a circq or a dynq only. An
 * fd selectable never reaches either function. ccol_selectable_from_chan
 * already resolved a channel selectable down to one of its two circular
 * queues before it gets here. */
static void *_queue_sel_queue(ccol_selectable *sel) {
  return (sel->type == ccol_selectable_circq) ? (void *)sel->cq
                                              : (void *)sel->dq;
}

static _Atomic size_t *_queue_sel_dispatch_refs(ccol_selectable *sel) {
  return (sel->type == ccol_selectable_circq) ? &sel->cq->dispatch_refs
                                              : &sel->dq->dispatch_refs;
}

/* Claims a reference to the queue that sel names. The destroy of that
 * queue then waits for this dispatch. Without the claim, that destroy frees
 * the queue out from under the dispatch. The caller holds the stripe lock
 * that also guards the reg->removed check that it just passed.
 * ccol_event_loop_remove stores that flag under the same lock. A removal
 * therefore has two possible positions. It happens first, and then the code
 * takes no reference at all. Or it happens second, and then it finds this
 * reference already published. Once remove returns, no further reference can
 * be claimed.
 *
 * Only a dispatch that is about to run its callback may claim it. The path
 * for num_reactor_threads == 1 claims it at collection time, because it runs
 * the callback directly after, on the same thread. The dispatch-pool path
 * claims it when the job starts on a worker, and never when the poller
 * collects the job. A queued job that holds a reference makes a destroy wait
 * for a job that may sit behind the very worker that runs that destroy. That
 * worker runs the callback of another registration that tears this queue
 * down, which the documentation permits. The wait then never ends, and it
 * takes the worker with it for good. A job that starts after the removal
 * finds reg->removed set, claims nothing and touches the queue not at all. */
static void _queue_dispatch_ref_acquire(ccol_selectable *sel) {
  atomic_fetch_add(_queue_sel_dispatch_refs(sel), (size_t)1);
}

/* Returns how many messages the queue that sel names holds. The caller holds
 * the mutex of that queue. */
static size_t _queue_sel_msg_count(ccol_selectable *sel) {
  return (sel->type == ccol_selectable_circq) ? sel->cq->msg_count
                                              : sel->dq->msg_count;
}

/* Consumes the outstanding wake of a queue-backed registration: it clears
 * reg->wait_ready and drains reg->bridge_efd in one critical section under
 * reg->wait_mtx. _notify_waiter sets the flag and writes the eventfd in one
 * critical section under the same mutex, so a notify either lands before
 * this call, and its message is already in the queue for the callback that
 * follows, or it lands after, finds the flag clear and writes the eventfd
 * again. The dispatch that follows the next epoll_wait then covers it.
 *
 * When queue_count is not NULL, the queue is alive (reg is not removed) and
 * the function also stores the message count of the queue in *queue_count.
 * It reads that count under the queue mutex and takes wait_mtx before it
 * releases the queue mutex. Every send or receive after the snapshot then
 * holds the queue mutex after this call, so its notify runs after the clear
 * and leaves a wake pending. The cascade step after the callback compares
 * the count against this snapshot to learn whether the dispatch moved any
 * message; see _ccol_event_loop_queue_cascade_notify_next. The queue mutex
 * is not held across the drain, so producers never wait on that read(2).
 * The function also stores in *hops the hop count of the wake that it
 * consumes (see the fwd_hops field of ccol_sel_waiter) and resets it; the
 * cascade step judges a dispatch that moved nothing by it.
 *
 * The caller holds the stripe lock of reg; the order stripe lock, queue
 * mutex, wait_mtx agrees with every other path. */
static void _queue_reg_consume_wake(ccol_event_reg_s *reg, size_t *queue_count,
                                    uint32_t *hops) {
  if (queue_count) {
    ccol_mutex_t *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
    ccol_mutex_lock(*q_mtx);
    *queue_count = _queue_sel_msg_count(&reg->sel);
    ccol_mutex_lock(reg->wait_mtx);
    ccol_mutex_unlock(*q_mtx);
  } else {
    ccol_mutex_lock(reg->wait_mtx);
  }
  reg->wait_ready = false;
  if (hops) *hops = reg->waiter_node.fwd_hops;
  reg->waiter_node.fwd_hops = 0;
  _eventfd_drain(reg->bridge_efd);
  ccol_mutex_unlock(reg->wait_mtx);
}

/* Releases a reference that _queue_dispatch_ref_acquire claimed. Nothing on
 * this path may touch the queue after this call. A destroy that blocks on the
 * counter can go on the instant that this function returns. */
static void _queue_dispatch_ref_release(ccol_selectable *sel) {
  atomic_fetch_sub(_queue_sel_dispatch_refs(sel), (size_t)1);
}

/* The per-loop slot table entry below the public ccol_event_reg handle value.
 * See the reg_slots field comment of struct ccol_event_loop_s for the
 * full design.
 */
typedef struct {
  ccol_event_reg_s *ptr; /* NULL when the slot is free */
  uint32_t generation;   /* The code mints it fresh on every acquire. It rises
                             for each slot index. It is 0 before the first
                             use, and it becomes 1 on the first acquire. */
  bool in_use;
} ccol_event_reg_slot_t;

#ifdef RUNNING_UNIT_TESTS
/* Test-only. It forces the next _ccol_event_reg_slot_acquire call to report a
 * failure. The call then behaves as if the growth allocation of
 * loop->reg_slots failed. This works whichever allocator the loop really
 * uses. The hook then disarms itself.
 *
 * The allocator parameter of a public constructor cannot reliably force a
 * failure at exactly this one call. loop->reg_slots and reg_free_indices both
 * get a non-zero minimum capacity at loop-creation time. See the "min
 * capacity 4" policy of cvector. An ordinary ccol_event_loop_add call
 * therefore reaches a real growth-triggering reallocation only once a loop
 * already holds several live registrations. Even then, the SAME per-loop
 * allocator serves every other allocation of this module. Those include the
 * chmap of the fd registry and dispatch_pool. Fault injection alone therefore
 * cannot fail this one call in isolation. This hook mirrors the established
 * precedent of this codebase for this class of hard-to-reach
 * allocation-failure test. See
 * clog_test_force_next_fresh_slot_registration_failure in clogger.c. */
static _Atomic bool g_force_next_reg_slot_acquire_failure = false;

/* A snapshot of loop->reg_count. The code takes it at the exact moment where
 * a forced failure above fires, and before it returns to the caller. This is
 * what lets a test prove the ORDERING of ccol_event_loop_add directly and
 * deterministically. That order is: acquire the slot first, and only then
 * wire the reg into the registry. Without the snapshot, a test can only see
 * the identical end state of the two orders. Both make ccol_event_loop_add
 * return CCOL_EVENT_REG_INVALID with reg_count back at 0, because the
 * rollback of a post-wiring failure also restores reg_count. reg_count can
 * only be already incremented for the registration of THIS call when the
 * wiring ran before the code even tried the slot acquire. A snapshot of 0
 * here therefore proves that the wiring did not happen yet. The code does not
 * reset this value between calls, unlike the one-shot force flag above. A
 * test that never armed the force flag never reads this accessor. */
static _Atomic size_t g_forced_slot_acquire_failure_reg_count_snapshot = 0;

void ccol_event_loop_test_force_next_reg_slot_acquire_failure(void) {
  atomic_store(&g_force_next_reg_slot_acquire_failure, true);
}

size_t ccol_event_loop_test_last_forced_slot_acquire_failure_reg_count(void) {
  return atomic_load(&g_forced_slot_acquire_failure_reg_count_snapshot);
}

/* Called by ccol_event_loop_add once the new registration is live and before
 * the handle goes back to the caller. NULL means no hook. */
static void (*_Atomic g_evl_add_before_return_hook)(void) = NULL;

void ccol_event_loop_test_set_add_before_return_hook(void (*hook)(void)) {
  atomic_store(&g_evl_add_before_return_hook, hook);
}

/* Test-only. These two functions lock and unlock the write side of the
 * reg_slot_rwlock of loop directly. They go around every public API function.
 * They mirror the established pattern of
 * ccol_circq_test_lock_mutex_for_tests. A test can hold this write side
 * locked from a thread OTHER than the one that will call fork(). It can hold
 * it for a window of any length that it controls precisely. That is the
 * scenario that deterministically exercises the write-lock reinit for
 * reg_slot_rwlock in the in_child branch of _cthreadcomm_atfork_release_impl:
 * prepare() waits for that window to end, takes the write side itself, and
 * the child, whose one thread has a new TID, cannot release it with a plain
 * ccol_rw_lock_unlock.
 *
 * The lock call resolves loop exactly ONCE, on purpose. It keeps the pin
 * of that resolve for as long as the write lock stays held, and does not
 * release it. The later unlock call therefore does not resolve loop again. A
 * second _ccol_event_loop_resolve call from the unlock side needs the
 * process-wide mutex of ccol_event_loop_slot_table. The forking thread
 * already holds that mutex, from before Phase 1 of
 * _cthreadcomm_atfork_prepare. It holds it for as long as it is itself
 * blocked and waits for THIS write lock. That is a real AB-BA deadlock, and
 * not a theoretical one. The forking thread ends up stuck in the
 * ccol_rw_lock_wrlock of _cthreadcomm_atfork_prepare. At the same time the
 * holder thread is stuck in its resolve of loop for its own unlock call.
 *
 * The lock call therefore passes the resolved struct ccol_event_loop_s*
 * through to the unlock call as an opaque pointer. That removes the second
 * resolve completely. Every real public API function here already does the
 * same thing. Each one resolves once for each call. It then holds raw for
 * the whole duration of that call. The only difference is the split. This
 * test hook has one logical operation, which is to hold the lock across a
 * window that the test times from outside. The code splits that operation
 * across two separate calls for the convenience of the test. */
void *ccol_event_loop_test_wrlock_reg_slot_for_tests(ccol_event_loop loop) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return NULL;
  ccol_rw_lock_wrlock(raw->reg_slot_rwlock);
  return raw;
}

void ccol_event_loop_test_wrunlock_reg_slot_for_tests(void *resolved_loop) {
  struct ccol_event_loop_s *raw = (struct ccol_event_loop_s *)resolved_loop;
  if (!raw) return;
  ccol_rw_lock_unlock(raw->reg_slot_rwlock);
  _ccol_event_loop_resolve_unpin(raw);
}
#endif

/* Allocates a fresh slot in loop->reg_slots for reg, or reuses a freed one.
 * It returns the ccol_event_reg handle that results, or 0 when it cannot
 * allocate. ccol_event_loop_add calls it BEFORE it wires reg into the fd or
 * queue registry. See the comment of that function for the reason why
 * this order, and not the reverse, keeps a failed slot acquire cheap to
 * unwind. The constructor of ccol_event_loop is different: a failed slot
 * acquire there must unwind a poller thread that already runs. A reg that the
 * code did not wire into any registry yet has nothing else to unwind. This
 * function sets reg->self_slot_idx. The caller must not hold
 * loop->reg_slot_rwlock. The function takes the write side of that lock. It
 * changes reg_slots and reg_free_indices. The cvector_push_back call below
 * can also reallocate the backing storage of reg_slots. A concurrent
 * _ccol_event_reg_resolve reader must never see that storage mid-move. */
#ifdef RUNNING_UNIT_TESTS
/* Test-only. It forces the next free-index push in
   _ccol_event_reg_slot_release to report a failure. It then disarms itself.
   No caller-supplied allocator can force a failure at exactly that one call.
   reg_free_indices grows to a non-zero capacity at loop creation, and a push
   into spare capacity never allocates. */
static _Atomic bool g_fail_next_reg_free_index_push = false;

void _ccol_event_loop_force_next_reg_free_index_push_failure_for_tests(void) {
  atomic_store(&g_fail_next_reg_free_index_push, true);
}

/* Test-only. It gives how many registration slots this loop ever allocated.
   A recovered lost index must not make this number grow. */
size_t _ccol_event_loop_reg_slot_count_for_tests(ccol_event_loop loop) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return 0;
  ccol_rw_lock_rdlock(raw->reg_slot_rwlock);
  size_t n = cvector_elem_count(raw->reg_slots);
  ccol_rw_lock_unlock(raw->reg_slot_rwlock);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}
#endif

/* Recovers one slot index that _ccol_event_reg_slot_release could not push
   back onto reg_free_indices. See loop->reg_lost_indices. A caller must hold
   the write side of reg_slot_rwlock. It must also have found
   reg_free_indices empty. That emptiness is what makes the predicate
   unambiguous. A released slot is either on the free list or lost, and never
   both. A slot that is still in use carries a non-NULL ptr. The function
   stays noinline, so the ordinary acquire path keeps its own code shape. That
   path only evaluates the reg_lost_indices comparison. The function zeroes
   the counter when a scan finds nothing. A wrong count can therefore never
   make every later acquire pay for a scan that cannot succeed. */
static __attribute__((noinline)) bool _ccol_event_reg_reclaim_lost_index_locked(
    struct ccol_event_loop_s *loop, uint32_t *out_idx) {
  size_t slot_count = cvector_elem_count(loop->reg_slots);
  for (size_t i = 0; i < slot_count; i++) {
    ccol_event_reg_slot_t *slot =
        (ccol_event_reg_slot_t *)cvector_at(loop->reg_slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      loop->reg_lost_indices--;
      return true;
    }
  }
  loop->reg_lost_indices = 0;
  return false;
}

static ccol_event_reg _ccol_event_reg_slot_acquire(
    struct ccol_event_loop_s *loop, ccol_event_reg_s *reg) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_force_next_reg_slot_acquire_failure)) {
    atomic_store(&g_force_next_reg_slot_acquire_failure, false);
    atomic_store(&g_forced_slot_acquire_failure_reg_count_snapshot,
                 atomic_load(&loop->reg_count));
    return 0;
  }
#endif
  ccol_rw_lock_wrlock(loop->reg_slot_rwlock);
  uint32_t idx;
  ccol_event_reg_slot_t *slot;
  if (cvector_elem_count(loop->reg_free_indices) > 0) {
    cvector_pop_back(loop->reg_free_indices, &idx);
    slot = (ccol_event_reg_slot_t *)cvector_at(loop->reg_slots, idx);
  } else if (loop->reg_lost_indices > 0 &&
             _ccol_event_reg_reclaim_lost_index_locked(loop, &idx)) {
    slot = (ccol_event_reg_slot_t *)cvector_at(loop->reg_slots, idx);
  } else {
    ccol_event_reg_slot_t fresh = {0};
    if (cvector_push_back(loop->reg_slots, &fresh) != ccol_success) {
      ccol_rw_lock_unlock(loop->reg_slot_rwlock);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(loop->reg_slots) - 1;
    slot = (ccol_event_reg_slot_t *)cvector_at(loop->reg_slots, idx);
  }
  slot->generation++;
  /* This skips the one generation value that collides with the reserved
   * "invalid handle" sentinel, which is 0. That collision comes after about
   * 2^32 reuses of this exact slot index. See the identical guard in
   * _ccol_event_loop_handle_slot_acquire for the full reasoning. */
  if (slot->generation == 0) slot->generation++;
  slot->ptr = reg;
  slot->in_use = true;
  reg->self_slot_idx = idx;
  ccol_event_reg h =
      ((ccol_event_reg)idx << 32) | (ccol_event_reg)slot->generation;
  reg->self_handle = h;
  ccol_rw_lock_unlock(loop->reg_slot_rwlock);
  return h;
}

/* Resolves h against the reg slot table of loop. It pins the result
 * against a concurrent free. It returns NULL in three cases. The first is an
 * h of 0. The second is garbage. The third is a slot that is free now, or
 * that an earlier acquire already reused, which gives the wrong generation.
 * On success the caller MUST call _ccol_event_reg_resolve_unpin(result)
 * exactly once. It must make that call as soon as it stops touching the
 * resolved ccol_event_reg_s*. This function mirrors
 * _ccol_event_loop_resolve exactly. It covers the table of one loop
 * instead of the process-wide one.
 *
 * It takes the read side of reg_slot_rwlock. It only reads the index, the
 * generation and ptr, and it never changes the table. Concurrent resolves can
 * therefore run together instead of serialising behind one lock. Those
 * resolves are the hot path. ccol_event_loop_pause, _resume and _modify call
 * them at the full request rate. See the field comment of
 * reg_slot_rwlock for the reason why this matters. The read side is still
 * mutually exclusive with _ccol_event_reg_slot_acquire and
 * _ccol_event_reg_slot_release, which take the write side. That is what stops
 * a reader here from ever seeing a reallocation of reg_slots mid-move. A
 * concurrent cvector_push_back triggers such a reallocation. */
static ccol_event_reg_s *_ccol_event_reg_resolve(struct ccol_event_loop_s *loop,
                                                 ccol_event_reg h) {
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol_rw_lock_rdlock(loop->reg_slot_rwlock);
  ccol_event_reg_s *raw = NULL;
  if (idx < cvector_elem_count(loop->reg_slots)) {
    ccol_event_reg_slot_t *slot =
        (ccol_event_reg_slot_t *)cvector_at(loop->reg_slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* The increment happens while the code STILL holds the read side of
   * reg_slot_rwlock. This matches the pending_resolve_count increment of
   * _ccol_event_loop_resolve exactly. Only one thing can make raw unsafe to
   * touch here: a slot that is already marked not in use.
   * ccol_event_loop_remove does that under the write side of this same
   * rwlock. The check above just confirmed that the slot is in use. The write
   * side can also never run at the same time as this read side. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  ccol_rw_lock_unlock(loop->reg_slot_rwlock);
  return raw;
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. It forces the next _ccol_event_reg_resolve_unpin call to sleep
 * for a duration that the caller chooses. That sleep sits between two steps.
 * The first step reads removed and handlers.on_removed, where the
 * still-held resolve pin guarantees that raw is safe to touch. The second
 * step decrements pending_resolve_count. The hook then disarms itself.
 *
 * A test can therefore hold a resolve pin open deterministically. It holds it
 * long enough for a concurrent ccol_event_loop_remove on a different thread
 * to mark the same reg removed and to defer its free first. This widens the
 * read-before-decrement window of maybe_needs_wake. See the comment of
 * that variable inside _ccol_event_reg_resolve_unpin. That window is
 * ordinarily tiny, and the hook makes it big enough for a test to land a
 * concurrent ccol_event_loop_remove call inside it. The test then reaches one
 * case every time: the snapshot goes stale, and the bounded retry of
 * EVENT_LOOP_RECLAIM_RETRY_MS reclaims the reg and fires on_removed. The
 * ping of this unpin does not. */
static _Atomic uint32_t g_delay_next_reg_resolve_unpin_ms = 0;

void ccol_event_loop_test_delay_next_reg_resolve_unpin_ms(uint32_t ms) {
  atomic_store(&g_delay_next_reg_resolve_unpin_ms, ms);
}
#endif

static void _ccol_event_reg_resolve_unpin(struct ccol_event_loop_s *loop,
                                          ccol_event_reg_s *raw) {
  /* This read comes before the decrement below. The still-held pin of
   * this call unconditionally guarantees that raw is safe to touch here.
   * removed and handlers.on_removed matter for one decision only: whether to
   * ping reclaim_wake_efd afterwards. See the comment of that decision
   * below. The decrement below can make raw eligible for a concurrent free,
   * and after that point a read of either field is no longer safe. A removed
   * of false here keeps this one cheap load, with no syscall. That is the
   * overwhelmingly common case of an unpin for a registration that is still
   * live. ccol_event_loop_modify, _pause, _resume and
   * ccol_event_loop_reg_generation are all far hotter call paths than
   * ccol_event_loop_remove in practice. Each of them unpins a raw that is
   * almost always still live. */
  bool maybe_needs_wake =
      atomic_load(&raw->removed) && raw->handlers.on_removed;

#ifdef RUNNING_UNIT_TESTS
  /* See the comment of g_delay_next_reg_resolve_unpin_ms. The code puts
   * this after the read above and before the decrement below. The pin of
   * this call is still held in either position. This matches exactly what a
   * real but unusually slow caller looks like from the point of view of
   * this function. Such a caller runs ccol_event_loop_modify, _pause, _resume
   * or ccol_event_loop_reg_generation. */
  uint32_t delay_ms = atomic_exchange(&g_delay_next_reg_resolve_unpin_ms, 0u);
  if (delay_ms > 0) {
    struct timespec ts = {.tv_sec = delay_ms / 1000,
                          .tv_nsec = (long)(delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
  }
#endif

  /* A bare atomic decrement, with no lock.
   * _ccol_event_loop_reclaim_pending_frees polls this value. It does not wait
   * on a condition variable. There is therefore no lost-wakeup hazard to
   * guard against. This mirrors the identical reasoning of
   * _ccol_event_loop_resolve_unpin exactly. */
  int prev = atomic_fetch_sub(&raw->pending_resolve_count, 1);

  /* A best-effort latency optimization only. It is not the correctness
   * mechanism. It pings reclaim_wake_efd. See the field comment of that
   * fd. It pings when this unpin LOOKS like it just made a reg newly eligible
   * for reclaim. Such a reg is already removed and carries an on_removed
   * handler. The early snapshot of maybe_needs_wake is the only evidence.
   *
   * The code reads maybe_needs_wake before the decrement above for one
   * reason: raw is only guaranteed safe to touch until that point. See the
   * comment of this function above. A concurrent ccol_event_loop_remove
   * can land strictly between that read and the decrement.
   * ccol_event_loop_modify, _pause and _resume all do genuine work between
   * the resolve and the unpin, and not only a couple of instructions. That
   * landing is therefore a real possibility. Such a removal makes this
   * snapshot stale. This
   * condition can therefore miss the one case where a ping matters most.
   *
   * A read of the fields of raw AFTER the decrement closes that staleness. It
   * also reopens the exact use-after-free that this pin exists to prevent.
   * The unconditional ping of ccol_event_loop_remove in
   * _ccol_event_loop_remove_finish can already have woken poller_thread. That
   * thread can then free raw between the decrement of this call and any later
   * read of raw.
   *
   * The bounded epoll_wait retry of EVENT_LOOP_RECLAIM_RETRY_MS instead
   * guarantees correctness for the case that this ping misses. See
   * _ccol_event_loop_reclaim_pending_frees and _ccol_event_loop_thread_fn.
   * That retry needs no snapshot of raw at all. A stale miss here costs at
   * most one extra retry interval of latency, and never an indefinite
   * hang. */
  if (prev == 1 && maybe_needs_wake) _eventfd_notify(loop->reclaim_wake_efd);
}

/* Marks the slot of reg as not in use and raises its generation. Every
 * future _ccol_event_reg_resolve of the handle value that
 * ccol_event_loop_add returned for it then fails at once. The function also
 * pushes the index onto the free list, so a future reg can reuse it.
 * ccol_event_loop_remove calls it at the exact point where the code removes
 * reg logically. That point is independent of, and always well before, the
 * real free of the ccol_event_reg_s struct itself. See
 * _ccol_event_loop_reclaim_pending_frees. The slot and the struct have
 * separate lifecycles, and only reg->self_slot_idx links them. A brand new
 * registration can therefore reuse this slot index the moment that this
 * function returns. That reuse can never race the still-pending deferred
 * free of the original reg. */
static void _ccol_event_reg_slot_release(struct ccol_event_loop_s *loop,
                                         ccol_event_reg_s *reg) {
  ccol_rw_lock_wrlock(loop->reg_slot_rwlock);
  ccol_event_reg_slot_t *slot =
      (ccol_event_reg_slot_t *)cvector_at(loop->reg_slots, reg->self_slot_idx);
  slot->in_use = false;
  slot->ptr = NULL;
  /* A failed push leaves this index released but off the free list. The code
     counts it, so the acquire path can recover it. Without the count, the
     slot is lost for as long as this loop lives. */
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_fail_next_reg_free_index_push, false)) {
    loop->reg_lost_indices++;
    ccol_rw_lock_unlock(loop->reg_slot_rwlock);
    return;
  }
#endif
  if (cvector_push_back(loop->reg_free_indices, &reg->self_slot_idx) !=
      ccol_success) {
    loop->reg_lost_indices++;
  }
  ccol_rw_lock_unlock(loop->reg_slot_rwlock);
}

static ccol_event_reg_s *_ccol_event_reg_create(struct ccol_event_loop_s *loop,
                                                ccol_selectable sel,
                                                ccol_event_handlers_t handlers,
                                                void *arg) {
  ccol_event_reg_s *reg =
      _ccol_mem_calloc(loop->m_procs, 1, sizeof(ccol_event_reg_s));
  if (!reg) return NULL;
  reg->sel = sel;
  reg->handlers = handlers;
  reg->arg = arg;
  atomic_init(&reg->refcount, 1);
  atomic_init(&reg->removed, false);
  atomic_init(&reg->paused, false);
  atomic_init(&reg->pending_resolve_count, (size_t)0);
  reg->bridge_efd = -1;
  /* The code initialises wait_mtx and wait_cond here. It does this
   * unconditionally for every non-fd registration, which means every queue
   * and ccol_channel registration, and exactly once. It does this whether or
   * not _ccol_event_loop_add_queue ever runs to completion. The
   * event_entry allocation of ccol_event_loop_add can fail and return before
   * the code ever calls _ccol_event_loop_add_queue. This is what lets
   * _ccol_event_reg_free be the SOLE owner of the destroy of these two, on
   * every path. Those paths are a later, successful removal and any failure
   * at registration time. It carries no risk of a second
   * pthread_mutex_destroy or pthread_cond_destroy, which POSIX makes
   * undefined behaviour. It also carries no risk of a destroy of memory that
   * nothing initialised. See the comment of _ccol_event_reg_free. */
  if (sel.type != ccol_selectable_fd) {
    /* On either failure the code frees reg directly, and not through
     * _ccol_event_reg_free. Nothing validly initialised wait_mtx and
     * wait_cond at that point. The ccol_mutex_destroy and
     * ccol_cond_var_destroy calls of that function on them are therefore
     * undefined behaviour. On every OTHER path, _ccol_event_reg_free must be
     * the sole, exclusive owner of their teardown. See the doc comment
     * of this function above for the reason. */
    if (ccol_mutex_init(reg->wait_mtx) != 0) {
      _ccol_mem_free(loop->m_procs, reg);
      return NULL;
    }
    if (ccol_cond_var_init(reg->wait_cond) != 0) {
      ccol_mutex_destroy(reg->wait_mtx);
      _ccol_mem_free(loop->m_procs, reg);
      return NULL;
    }
  }
  return reg;
}

/* This function is the single owner of two teardown steps for reg. It
 * destroys wait_mtx and wait_cond, which _ccol_event_reg_create above
 * initialised. It also closes bridge_efd, which _ccol_event_loop_add_queue
 * opened. The code calls it in two situations. The first is the final free of
 * a live registration, after its removal and once the deferred-free
 * reclamation allows it. The second is a ccol_event_loop_add call that fails
 * part way through the registration of a queue or ccol_channel selectable.
 * The failure paths of _ccol_event_loop_add_queue must not destroy these
 * fields, and neither may any other function. One destroy here, exactly once
 * and whichever caller reaches this function, is what makes a double destroy
 * impossible. Without that rule, a teardown in the failure paths of
 * _ccol_event_loop_add_queue lets this function tear the same fields down a
 * second time directly afterwards. */
/* The code sets reg->owning_entry exactly once. It sets it only on the
 * success path of _ccol_event_loop_add_fd or _ccol_event_loop_add_queue. See
 * the tail of either function. Nothing clears it afterwards. The field is
 * therefore still NULL at every call site of this function that tears down a
 * reg that ccol_event_loop_add never wired live. No dispatch can ever have
 * seen such a reg, so the owner of arg has nothing to learn about. The field
 * is non-NULL at every call site that frees a reg that WAS live at some
 * point. That covers an ordinary removal that reaches
 * _ccol_event_loop_reclaim_pending_frees once its refcount and its
 * pending_resolve_count both reach 0. It also covers a still-registered reg
 * that the teardown walk of __ccol_event_loop_destroy sweeps up. This is
 * what lets on_removed fire exactly once for each registration that ever
 * became real, and never for one that did not. One check right here does it,
 * and no call site needs to know which category it is in.
 *
 * Every call site reaches this function while it holds none of the locks
 * of this module. The collection and dispatch locks are entry-scoped, and
 * nothing here touches them. The bookkeeping of reg, wait_mtx and
 * wait_cond, is only ever destroyed below, and never locked. on_removed is
 * therefore free to take an application-level lock of its own. That includes
 * a lock that the other callbacks of this same registration also take. It
 * carries no risk of a lock-order cycle against anything in this module. */
static void _ccol_event_reg_free(struct ccol_event_loop_s *loop,
                                 ccol_event_reg_s *reg) {
  if (reg->owning_entry && reg->handlers.on_removed)
    reg->handlers.on_removed(reg->arg);

  if (reg->sel.type != ccol_selectable_fd) {
    ccol_mutex_destroy(reg->wait_mtx);
    ccol_cond_var_destroy(reg->wait_cond);
    if (reg->bridge_efd >= 0) close(reg->bridge_efd);
  }
  _ccol_mem_free(loop->m_procs, reg);
}

/* Gives the epoll interest bits that one direction of an fd entry adds to the
 * combined mask of that entry. It gives 0 when the direction has no
 * registration at all, and also when its registration is paused now. dir is
 * the direction that the caller asks about. It is always the slot that the
 * caller read reg out of.
 *
 * Only a registration with a real on_readable handler adds EPOLLIN. Only a
 * registration with a real on_writable handler adds EPOLLOUT. epoll is
 * level-triggered here. A readiness that nothing can consume comes back on
 * every single epoll_wait call, for ever, and the reactor burns a whole
 * thread on it. At the default num_reactor_threads of 1 that is the sole
 * thread, and every other registration on the loop starves too.
 *
 * An error-only registration is exactly that case. It has on_error set and
 * the handler of the direction NULL. That is how a caller says "tell me
 * when this fd dies, and I want to neither read it nor write it". A healthy
 * connected socket is writable from the instant of its registration. EPOLLOUT
 * for it therefore spins with no action from the peer at all.
 *
 * The function adds EPOLLERR and EPOLLHUP unconditionally. epoll reports them
 * for a registered fd whether or not the mask asks for them. For a read
 * direction the function adds EPOLLRDHUP beside them, so a peer that closes
 * its end still reaches on_error. A plain TCP close() raises EPOLLIN and
 * EPOLLRDHUP without EPOLLHUP, because EPOLLHUP needs both directions down.
 * EPOLLRDHUP is therefore the only bit that reports it to a registration with
 * no reader. */
static uint32_t _fd_reg_interest_bits(ccol_event_reg_s *reg,
                                      ccol_select_dir dir) {
  if (!reg || atomic_load(&reg->paused) || reg->muted) return 0;
  uint32_t bits = (uint32_t)(CCOL_POLL_ERR | CCOL_POLL_HUP);
  if (dir == ccol_select_read) {
    bits |= (uint32_t)CCOL_POLL_RDHUP;
    if (reg->handlers.on_readable) bits |= (uint32_t)CCOL_POLL_IN;
  } else if (reg->handlers.on_writable) {
    bits |= (uint32_t)CCOL_POLL_OUT;
  }
  return bits;
}

/* The byte size of one event_entry allocation for loop. A loop with a
 * dispatch_pool gives every entry the storage of its own dispatch job as the
 * tail of the same block. See _ccol_event_loop_entry_job. The definition sits
 * below the type of that job. */
static size_t _ccol_event_loop_entry_alloc_size(
    const struct ccol_event_loop_s *loop);

/* Registers a new fd direction for reg. Its stripe is idx. The caller,
 * ccol_event_loop_add, computed idx from sel.fd before it took any lock.
 * sel.fd can already have an event_entry, which means that its other
 * direction is already registered. The function then combines the interest
 * with EPOLL_CTL_MOD. In every other case it creates a fresh entry with
 * EPOLL_CTL_ADD. It rejects a direction that a different reg already occupies
 * and returns ccol_not_permitted. On success it sets reg->owning_entry and
 * reg->stripe_idx. For a new entry it also sets entry->stripe_idx. The caller
 * must already hold loop->stripes[idx].lock. */
static ccol_retval_t _ccol_event_loop_add_fd(struct ccol_event_loop_s *loop,
                                             size_t idx,
                                             ccol_event_reg_s *reg) {
  ccol_event_loop_stripe_t *stripe = &loop->stripes[idx];
  int fd = reg->sel.fd;
  event_entry *entry = _fd_registry_find(stripe, fd);
  bool new_entry = (entry == NULL);

  if (new_entry) {
    entry = _ccol_mem_calloc(loop->m_procs, 1,
                             _ccol_event_loop_entry_alloc_size(loop));
    if (!entry) return ccol_not_enough_memory;
    entry->is_fd = true;
    entry->fd = fd;
    entry->stripe_idx = idx;
    entry->as.fd.read_reg = NULL;
    entry->as.fd.write_reg = NULL;
    if (ccol_mutex_init(entry->dispatch_lock) != 0) {
      _ccol_mem_free(loop->m_procs, entry);
      return ccol_unexpected_failure;
    }
    atomic_init(&entry->removed, false);
    atomic_init(&entry->refcount, (size_t)0);
    /* The code mints this once for each NEW entry. It never mints one for a
     * second direction that joins an fd that is already registered. That case
     * takes the !new_entry path below and shares the generation of the
     * existing entry. That is correct, because both directions stand for one
     * logical connection. */
    entry->generation = atomic_fetch_add(&loop->fd_generation_counter, 1) + 1;
  }

  ccol_event_reg_s **slot = (reg->sel.dir == ccol_select_read)
                                ? &entry->as.fd.read_reg
                                : &entry->as.fd.write_reg;
  if (*slot != NULL) {
    if (new_entry) {
      ccol_mutex_destroy(entry->dispatch_lock);
      _ccol_mem_free(loop->m_procs, entry);
    }
    return ccol_not_permitted;
  }
  *slot = reg;

  /* EPOLLONESHOT applies only when dispatch_pool exists, which means
   * num_reactor_threads > 1. In that configuration the collection, on
   * poller_thread, and the dispatch, on a ctpool worker, are decoupled.
   * Without EPOLLONESHOT, poller_thread sees a still-ready fd that nothing
   * dispatched yet on every later epoll_wait call. It sees it for as long as
   * the backlog lasts, and it mints an unbounded stream of redundant dispatch
   * jobs. That is a real resource-exhaustion and livelock risk under
   * sustained load, and not a rare corner case.
   * _ccol_event_loop_rearm_entry_locked re-arms after each dispatch job.
   *
   * The code MUST NOT set EPOLLONESHOT for num_reactor_threads == 1. The
   * dispatch of that path, _ccol_event_loop_handle_event, never re-arms
   * anything. There the collection and the dispatch are one synchronous call
   * on the same thread, and nothing else can re-arm it. EPOLLONESHOT there
   * silently stops the delivery of every event for this fd after the first
   * one. */
  uint32_t mask = loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0;
  mask |= _fd_reg_interest_bits(entry->as.fd.read_reg, ccol_select_read);
  mask |= _fd_reg_interest_bits(entry->as.fd.write_reg, ccol_select_write);

  ccol_poll_event ev;
  ev.data.ptr = entry;
  ev.events = mask;
  /* The choice keys off entry->epoll_added, and not off new_entry.
   * entry->epoll_added says whether the fd is CURRENTLY in the interest set
   * of loop->epfd. An earlier _ccol_event_loop_rearm_entry_locked call can
   * have removed an existing entry with EPOLL_CTL_DEL. It does that when
   * every one of the then-live directions of that entry is paused. See the
   * field comment of event_entry.epoll_added. A second direction that
   * joins the entry in that state needs EPOLL_CTL_ADD and not
   * EPOLL_CTL_MOD. A MOD on an fd that is not registered now fails with
   * ENOENT. The reg that the code just put into *slot above is always freshly
   * not paused. See _ccol_event_reg_create. mask therefore always carries
   * real interest bits here, whatever the paused state of a sibling
   * direction is. */
  /* A new entry goes into the fd index BEFORE epoll_ctl publishes it. The
   * index is read only under the stripe lock that this function holds, so
   * the insert is invisible until this call returns, and the one step here
   * that can fail for lack of memory runs while nothing else can reach the
   * entry. Once epoll_ctl has added the fd, the poller can already hold the
   * entry pointer from an epoll_wait batch, and only the deferred-free path
   * of a removal may free it. An immediate free after the publish is a
   * use-after-free on the poller. */
  if (new_entry && !_fd_registry_insert(stripe, fd, entry)) {
    *slot = NULL;
    ccol_mutex_destroy(entry->dispatch_lock);
    _ccol_mem_free(loop->m_procs, entry);
    return ccol_not_enough_memory;
  }

  int ctl_op = entry->epoll_added ? CCOL_POLL_CTL_MOD : CCOL_POLL_CTL_ADD;
  if (ccol_poll_ctl(loop->epfd, ctl_op, fd, &ev) < 0) {
    *slot = NULL;
    if (new_entry) {
      /* A failed EPOLL_CTL_ADD published nothing, so the entry is still
       * private to this call. */
      _fd_registry_remove(stripe, fd);
      ccol_mutex_destroy(entry->dispatch_lock);
      _ccol_mem_free(loop->m_procs, entry);
    }
    return ccol_unexpected_failure;
  }
  entry->epoll_added = true;

  reg->owning_entry = entry;
  reg->stripe_idx = idx;
  reg->generation = entry->generation;
  return ccol_success;
}

/* Peeks whether sel is ready now for its own direction. sel is a resolved
 * circq or dynq selectable, and never an fd. The function uses the one shared
 * definition of "ready" for a queue selectable in this file. For read that is
 * msg_count > 0. For write that is room to send, with the send turned on. The
 * caller must already hold the mutex of the resolved queue. This mirrors
 * every other reader of msg_count, max_size and writing_disabled in this
 * file. Two callers share this function: the already_ready check of
 * _ccol_event_loop_add_queue, and
 * _ccol_event_loop_queue_cascade_notify_next below. The two can therefore
 * never drift apart about what "ready" means. */
static bool _queue_sel_is_ready(ccol_selectable *sel) {
  if (sel->type == ccol_selectable_circq) {
    ccol_circular_queue *cq = sel->cq;
    return (sel->dir == ccol_select_read)
               ? cq->msg_count > 0
               : (cq->msg_count < cq->max_size && !cq->writing_disabled);
  }
  ccol_dynamic_queue *dq = sel->dq;
  return (sel->dir == ccol_select_read)
             ? dq->msg_count > 0
             : (!dq->writing_disabled && dq->msg_count < ccol_max_elem_count);
}

/* Sets up a queue-backed registration. It allocates the dedicated bridge
 * eventfd. It registers that eventfd with the persistent epoll instance of
 * the loop. It then links the embedded waiter_node of reg into the waiter
 * list of the queue, permanently. The per-call nodes of ccol_select are
 * transient instead. The existing notify_one_sel_waiter and
 * notify_all_sel_waiters of the queue therefore wake this registration too.
 * ccol_circq_send_zc, ccol_circq_recv_zc, ccol_dynmq_send_zc,
 * ccol_dynmq_recv_zc and the enable_sending and disable_sending functions
 * already call those two. */
static ccol_retval_t _ccol_event_loop_add_queue(struct ccol_event_loop_s *loop,
                                                event_entry *entry,
                                                ccol_event_reg_s *reg) {
  reg->bridge_efd = ccol_wakefd_create();
  if (reg->bridge_efd < 0) {
    /* _ccol_event_reg_create already initialised reg->wait_mtx and
     * reg->wait_cond. The code must NOT tear them down here. The caller is
     * ccol_event_loop_add. It unconditionally calls _ccol_event_reg_free(reg)
     * on any return from this function that is not a success. That function
     * is their one and only owner. A destroy here too is a second
     * pthread_mutex_destroy and pthread_cond_destroy, which POSIX makes
     * undefined behaviour. */
    return ccol_unexpected_failure;
  }
  reg->waiter_node.efd = reg->bridge_efd;

  /* See the identical comment in _ccol_event_loop_add_fd about EPOLLONESHOT
   * only when dispatch_pool exists. It applies here for exactly the same
   * reason, for one direction only. Two regs never share a bridge eventfd, so
   * there is no combined-mask concern like the one that the fd case has. */
  ccol_poll_event ev;
  ev.data.ptr = entry;
  ev.events = CCOL_POLL_IN | (loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0);
  if (ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_ADD, reg->bridge_efd, &ev) < 0) {
    /* Only the fd itself needs a close here. Nothing ever handed it to epoll,
     * so nothing else will ever close it. The code leaves wait_mtx and
     * wait_cond alone, so _ccol_event_reg_free tears them down exactly once.
     * The reasoning is the same as in the eventfd() failure branch above.
     * The code also resets reg->waiter_node.efd beside reg->bridge_efd. A few
     * lines above, before this call, it held the same fd value that the code
     * just closed. A stale value there is harmless today. This failure path
     * never links the node into the waiter list of any queue. Nothing
     * therefore reads it before the code frees reg. The reset still keeps
     * reg->bridge_efd and reg->waiter_node.efd from ever disagreeing about
     * whether a real, open fd exists. */
    close(reg->bridge_efd);
    reg->bridge_efd = -1;
    reg->waiter_node.efd = -1;
    return ccol_unexpected_failure;
  }

  ccol_mutex_t *q_mtx;
  ccol_sel_waiter **q_head;
  ccol_sel_waiter **q_rotor; /* Unused here. A link of a brand-new node never
                              * needs to touch the rotor. See the doc
                              * comment of notify_one_sel_waiter. */
  _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
  (void)q_rotor;

  ccol_mutex_lock(*q_mtx);
  /* This is a call convention with an array of one element. The function
   * _sel_link_waiter indexes into nodes[i], because ccol_select_timed always
   * has a real array that its caller owns. ccol_event_loop has exactly one
   * standalone waiter_node for each registration. With
   * nodes = &reg->waiter_node, nodes[0] is reg->waiter_node. */
  _sel_link_waiter(0, &reg->waiter_node, &reg->wait_mtx, &reg->wait_cond,
                   &reg->wait_ready, q_head);
  /* A real fd is different. There an epoll_ctl(ADD) against a kernel object
   * that is already readable reaches the very next epoll_wait. epoll tracks
   * the live state of the resource, and not only its edge transitions.
   * This bridge eventfd rings only on a FUTURE notify_one_sel_waiter call.
   * Without the step below, a message that already sits in the queue before
   * this registration exists is missed completely until the next send. The
   * code therefore triggers the eventfd itself here when the queue is already
   * in the target state. It does this still under q_mtx, so the check agrees
   * with the link above. The next epoll_wait of the reactor thread then
   * picks it up. It dispatches through the completely standard drain and
   * try_recv path, so a callback still only ever runs from there. */
  bool already_ready = _queue_sel_is_ready(&reg->sel);
  /* The wake goes through _notify_waiter, like every other wake of this
   * node, so that wait_ready records it. A raw eventfd write here leaves the
   * flag clear, and the next send then writes a second, redundant wake. */
  if (already_ready) _notify_waiter(&reg->waiter_node);
  ccol_mutex_unlock(*q_mtx);

  return ccol_success;
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only instrumentation for the cascade step below. See
 * ccol_event_loop_test_delay_next_queue_cascade_us and
 * ccol_event_loop_test_queue_cascade_counts in cthreadcomm.h. These are
 * atomic because the cascade runs on poller_thread or on a dispatch worker
 * while a test thread reads them at the same time. */
static _Atomic uint32_t g_queue_cascade_test_delay_us = 0;
static _Atomic uint64_t g_queue_cascade_begun_for_tests = 0;
static _Atomic uint64_t g_queue_cascade_finished_for_tests = 0;
#endif

/* Decides who hears about a queue that is still ready for the direction of
 * reg once the callback of reg returned. By that point the callback of this
 * dispatch already had its chance to consume or to produce. When the queue
 * is no longer ready, nobody is woken.
 *
 * The decision is made for reg alone, from the progress of its own dispatch:
 *
 *   - The dispatch moved a message: the message count of the queue went down
 *     for a read registration, or up for a write registration, between the
 *     consume of the wake (see _queue_reg_consume_wake) and this call. The
 *     function then wakes reg itself again, so a registration that moves
 *     messages is dispatched again for as long as it moves them and the
 *     queue stays ready, exactly like a level-triggered fd. It also wakes the
 *     successor of reg in the list (the node linked directly after it), so a
 *     backlog also reaches the other listeners of the queue.
 *
 *   - The dispatch moved nothing: the function passes the wake to the
 *     successor of reg, or to the head of the list when reg is the last node,
 *     so that a listener which declines a ready queue never keeps a message
 *     from a sibling that would take it. The forwarded wake carries a hop
 *     count one higher than the wake that reg consumed (see the fwd_hops
 *     field of ccol_sel_waiter), and the function stops once that count
 *     reaches the length of the list. A wake therefore visits every listener
 *     at most once while none of them moves anything, and listeners that
 *     all decline a ready queue cannot wake each other for ever.
 *
 * Neither rule depends on the position of reg in the list. A registration
 * that moves messages keeps its own dispatches going whether it is the head,
 * the tail or in between, and an idle sibling can neither end nor absorb
 * them. Every wake has a bounded chain behind it: a re-wake of reg needs a
 * message that moved, and every forward without progress raises the hop
 * count, which only a fresh event (a send, a receive, a state change, a new
 * registration or a departing listener) resets.
 *
 * Another thread can move a message between the consume and this call. A
 * move in the direction of reg (a receive for a reader, a send for a writer)
 * counts as progress here, which costs reg at most one extra dispatch for
 * each such move. A move in the other direction can hide the progress of
 * reg from the count; that operation notifies a waiter of the direction of
 * reg, and a wake without progress then walks the list until it reaches a
 * listener that moves messages, reg included. A modify that changed the
 * direction of reg during the dispatch makes the count meaningless, so the
 * dispatch then counts as one that moved nothing.
 *
 * Both dispatch paths call this function once, directly after the
 * callback returns. Those paths are _ccol_event_loop_handle_event and
 * _ccol_event_loop_dispatch_job_fn. At that point the extra refcount from the
 * collection still pins reg, so nothing can free reg out from under this
 * call.
 *
 * The function is necessary, because a ccol_event_loop registration is not
 * like a ccol_select() waiter. A ccol_select() waiter is transient. It always
 * unlinks itself before it checks readiness again, and it forwards the wake
 * when it leaves a ready queue; see deregister_sel_waiter. The waiter_node of
 * a ccol_event_loop registration instead stays linked into the list
 * permanently, for as long as the registration lives. One eventfd write
 * covers every send until the next dispatch, so a callback that takes one
 * message for each call leaves the rest of a burst in the queue, and no
 * further send may come; without the re-wake that backlog stays in the queue
 * with nobody woken. And a wake that lands on a listener that declines the
 * queue ends there without the forward, while a sibling that would take the
 * message waits for a send that may never come.
 *
 * The function holds loop->stripes[reg->stripe_idx].lock across BOTH the "is
 * reg still registered" test and the whole q_mtx critical section. The
 * memory of the queue has no refcount of its own, so it can stop existing
 * between the two. A lock of a queue mutex needs the QUEUE to still be alive.
 * That is a strictly stronger requirement than reg being alive. The
 * refcount bump of the collection keeps reg alive here, fully independently
 * of the lifetime of the queue. The application owns the queue. The
 * documented lifetime discipline of this module for it is: call
 * ccol_event_loop_remove, then destroy the queue at once. See the doc
 * comment of ccol_circular_queue_destroy. An unlocked read of reg->removed
 * followed by an unprotected ccol_mutex_lock(*q_mtx) therefore locks freed
 * memory whenever that destroy lands between the two. The gap can be tiny and
 * it still happens, because the OS can deschedule this thread for an
 * arbitrarily long time at exactly that point.
 *
 * The stripe lock is what makes the pair indivisible. Two reasons must hold
 * together:
 *
 *   - The queue branch of _ccol_event_loop_remove_unlink runs its whole
 *     splice-and-mark-removed step under this same stripe lock. The removed
 *     state of reg can therefore not change while this function holds that
 *     lock. A concurrent ccol_event_loop_remove is in one of two states. It
 *     did not start, and then removed reads false and stays false here. Or it
 *     fully finished, and then removed reads true and nothing below runs.
 *
 *   - A removed of false means that reg is still linked into the
 *     sel_{read,write}_waiters_head list of the queue. That is exactly the
 *     condition under which __ccol_circular_queue_destroy and
 *     __ccol_dynamic_queue_destroy refuse to destroy a queue. They abort
 *     through ccol_assert BEFORE they destroy the mutex or free the struct.
 *     An application that races a destroy against this function can therefore
 *     not free the queue out from under the ccol_mutex_lock below. The assert
 *     traps it as the caller bug that it already is.
 *
 * The lock order is stripe->lock first, then the queue mutex, then the
 * wait_mtx of the target waiter inside _notify_waiter. This matches the
 * stripe-then-queue nesting of _ccol_event_loop_add_queue and of
 * _ccol_event_loop_remove_unlink. It also matches the queue-then-wait_mtx
 * nesting of _notify_waiter. The caller already holds entry->dispatch_lock,
 * and dispatch lock before stripe lock is the order that
 * _ccol_event_loop_handle_event itself uses. Nothing in this module ever
 * takes a queue mutex or a wait_mtx before a stripe lock. No cycle therefore
 * appears.
 *
 * A read of reg->waiter_node.next is only safe under the mutex of the
 * queue. The functions _sel_link_waiter and _sel_unlink_waiter_locked
 * already depend on that same invariant throughout this file. The
 * _sel_unlink_waiter_locked call of a concurrent ccol_event_loop_remove()
 * updates the pointers of the NEIGHBOURS of reg->waiter_node. It never resets
 * reg->waiter_node.next and .prev themselves once the code splices reg out.
 * A stale read of reg->waiter_node.next after that removal can therefore
 * dereference a node that is itself unlinked by then, and possibly freed.
 *
 * The removed reg does not need this for its own sake. A removal hands on a
 * wake that the removed reg may still hold itself; see
 * _ccol_event_loop_remove_unlink. This only matters for a dispatch that is
 * in flight and that the code collected before a concurrent removal
 * finished. */
static void _ccol_event_loop_queue_cascade_notify_next(
    struct ccol_event_loop_s *loop, ccol_event_reg_s *reg,
    ccol_select_dir dispatched_dir, size_t queue_count_before,
    uint32_t wake_hops) {
  ccol_event_loop_stripe_t *stripe = &loop->stripes[reg->stripe_idx];
  ccol_mutex_lock(stripe->lock);

#ifdef RUNNING_UNIT_TESTS
  /* Both sit inside the stripe lock. A test that sees the begun counter
   * therefore knows that this call already holds that lock. See their own
   * declarations in cthreadcomm.h. */
  atomic_fetch_add(&g_queue_cascade_begun_for_tests, (uint64_t)1);
  {
    uint32_t cascade_delay_us =
        atomic_exchange(&g_queue_cascade_test_delay_us, 0u);
    if (cascade_delay_us > 0) {
      struct timespec cascade_ts = {
          .tv_sec = cascade_delay_us / 1000000,
          .tv_nsec = (long)(cascade_delay_us % 1000000) * 1000L};
      nanosleep(&cascade_ts, NULL);
    }
  }
#endif

  /* A second, inner check of removed is not needed once the code holds this
   * lock. The function _ccol_event_loop_remove_unlink is the only writer of
   * reg->removed, and it holds this exact lock across that write. The value
   * that the code reads here can therefore not change under the block
   * below. */
  if (!atomic_load(&reg->removed)) {
    ccol_mutex_t *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
    /* This cascade step is independent of the round-robin rotor of the
     * queue, and it never advances that rotor. See the doc comment of
     * notify_one_sel_waiter for the reason why the rotor exists. It is
     * correct to leave the rotor untouched here. The rotor tracks who is due
     * for the next brand-new arrival. That is a different concern from the
     * check of this function, which asks whether there is a backlog to
     * surface now. */
    (void)q_rotor;

    ccol_mutex_lock(*q_mtx);
    if (_queue_sel_is_ready(&reg->sel)) {
      ccol_sel_waiter *self = &reg->waiter_node;
      bool moved = false;
      if (reg->sel.dir == dispatched_dir) {
        size_t count_now = _queue_sel_msg_count(&reg->sel);
        moved = (dispatched_dir == ccol_select_read)
                    ? count_now < queue_count_before
                    : count_now > queue_count_before;
      }
      if (moved) {
        _notify_waiter(self);
        if (self->next) _notify_waiter_forwarded(self->next, 1u);
      } else if (wake_hops < UINT32_MAX) {
        uint32_t next_hops = wake_hops + 1u;
        ccol_sel_waiter *target = self->next ? self->next : *q_head;
        if (target != self && _sel_list_longer_than(*q_head, next_hops))
          _notify_waiter_forwarded(target, next_hops);
      }
    }
    ccol_mutex_unlock(*q_mtx);
  }

#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_queue_cascade_finished_for_tests, (uint64_t)1);
#endif
  ccol_mutex_unlock(stripe->lock);
}

#ifdef RUNNING_UNIT_TESTS
void ccol_event_loop_test_delay_next_queue_cascade_us(uint32_t us) {
  atomic_store(&g_queue_cascade_test_delay_us, us);
}

void ccol_event_loop_test_queue_cascade_counts(uint64_t *begun,
                                               uint64_t *finished) {
  if (begun) *begun = atomic_load(&g_queue_cascade_begun_for_tests);
  if (finished) *finished = atomic_load(&g_queue_cascade_finished_for_tests);
}
#endif

/* Validates a ccol_selectable for ccol_event_loop_add. */
static ccol_retval_t _ccol_event_loop_validate_add_args(
    struct ccol_event_loop_s *loop, ccol_selectable *sel) {
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

ccol_event_reg ccol_event_loop_add(ccol_event_loop loop, ccol_selectable sel,
                                   ccol_event_handlers_t handlers, void *arg,
                                   char **err_str) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Invalid or stale ccol_event_loop handle");
    return CCOL_EVENT_REG_INVALID;
  }

  ccol_retval_t validate = _ccol_event_loop_validate_add_args(raw, &sel);
  if (validate != ccol_success) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Invalid arguments to ccol_event_loop_add");
    _ccol_event_loop_resolve_unpin(raw);
    return CCOL_EVENT_REG_INVALID;
  }

  ccol_event_reg_s *reg = _ccol_event_reg_create(raw, sel, handlers, arg);
  if (!reg) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate memory for ccol_event_reg");
    _ccol_event_loop_resolve_unpin(raw);
    return CCOL_EVENT_REG_INVALID;
  }

  /* The slot acquire happens BEFORE the code wires reg into the fd or queue
   * registry below, and never after it. The reverse order is a real, if
   * narrow, use-after-free. The code wires reg and then releases the lock of
   * the target stripe. reg is then fully live and dispatchable through the
   * raw ccol_event_reg_s* in the registry alone. The poller thread needs no
   * own public handle or slot of reg to collect and dispatch it. For
   * num_reactor_threads > 1, that dispatch can hand reg->arg to a ctpool
   * worker asynchronously.
   *
   * A slot acquire has one failure mode: an allocation failure that grows
   * loop->reg_slots. A failure after that point makes ccol_event_loop_add
   * report a failure to its caller. That caller believes that the code never
   * created a registration. It therefore has no reason to think that a
   * callback can already run, or already wait in a queue, against reg->arg.
   * It can free reg->arg at once. Two things are enough to reach that state.
   * The first is a coincidence of timing, where readiness arrives in the
   * brief window between the wiring and the slot acquire. The second is an
   * ordinary allocation failure. No further misuse is needed.
   *
   * An acquire of the slot first closes this completely. reg can only become
   * dispatchable once it already has a valid handle. A failed slot acquire
   * therefore means that nothing was ever wired into any registry. There is
   * then nothing to roll back beyond a release of the slot that the code just
   * acquired and a direct free of reg. See the rv != ccol_success branch
   * below.
   *
   * This order must also match the only other place where these two locks
   * nest. The function _cthreadcomm_atfork_prepare locks the
   * reg_slot_rwlock of every live loop, on its write side, through
   * _ccol_event_reg_slot_acquire below.
   * It does that in its own Phase 1, before it takes any stripe lock of that
   * loop, and therefore before stripe->lock is ever taken here. An acquire of
   * the slot before the lock of the stripe keeps this function consistent
   * with that existing reg_slot_rwlock before stripe->lock order. The other
   * way around adds a new stripe->lock before reg_slot_rwlock edge. That edge
   * races the existing one and risks an AB-BA deadlock against a concurrent
   * fork(). It is not merely a style choice. */
  ccol_event_reg h = _ccol_event_reg_slot_acquire(raw, reg);
  if (h == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate ccol_event_reg handle slot");
    _ccol_event_reg_free(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return CCOL_EVENT_REG_INVALID;
  }

  /* The code computes the stripe index before it takes any lock. It uses data
   * that it already has. For an fd selectable it uses sel.fd. That is a pure
   * function of the fd. A second ccol_event_loop_add call for the other
   * direction of that fd therefore computes the same stripe on its own. It
   * then finds the existing entry through the chmap of that stripe. For a
   * queue or ccol_channel selectable it uses the round-robin cursor
   * loop->next_queue_stripe. Those have no such consistency rule. See the
   * comment of ccol_event_loop_stripe_t. */
  size_t idx =
      (sel.type == ccol_selectable_fd)
          ? _stripe_index_for_fd(raw, sel.fd)
          : (atomic_fetch_add(&raw->next_queue_stripe, 1) % raw->num_stripes);
  ccol_event_loop_stripe_t *stripe = &raw->stripes[idx];

  ccol_mutex_lock(stripe->lock);

  ccol_retval_t rv;
  if (sel.type == ccol_selectable_fd) {
    rv = _ccol_event_loop_add_fd(raw, idx, reg);
  } else {
    event_entry *entry = _ccol_mem_calloc(
        raw->m_procs, 1, _ccol_event_loop_entry_alloc_size(raw));
    if (!entry) {
      rv = ccol_not_enough_memory;
    } else {
      entry->is_fd = false;
      entry->fd = -1;
      entry->stripe_idx = idx;
      entry->as.reg = reg;
      if (ccol_mutex_init(entry->dispatch_lock) != 0) {
        _ccol_mem_free(raw->m_procs, entry);
        rv = ccol_unexpected_failure;
      } else {
        atomic_init(&entry->removed, false);
        atomic_init(&entry->refcount, (size_t)0);
        /* A queue selectable and a ccol_channel selectable never share an
         * entry. The relation is one to one, and the code combines nothing.
         * Every ccol_event_loop_add call here therefore mints a fresh
         * generation. The fd path is different. Here there is no case where a
         * second direction joins the existing entry. */
        entry->generation =
            atomic_fetch_add(&raw->fd_generation_counter, 1) + 1;
        rv = _ccol_event_loop_add_queue(raw, entry, reg);
        if (rv == ccol_success) {
          reg->owning_entry = entry;
          reg->stripe_idx = idx;
          reg->generation = entry->generation;
          _loop_queue_list_add(stripe, reg);
        } else {
          ccol_mutex_destroy(entry->dispatch_lock);
          _ccol_mem_free(raw->m_procs, entry);
        }
      }
    }
  }

  if (rv == ccol_success) atomic_fetch_add(&raw->reg_count, 1);

  ccol_mutex_unlock(stripe->lock);

  if (rv != ccol_success) {
    /* One message for each failure class, so that a caller can tell a
     * direction that another registration already holds (a usage error that
     * no retry fixes) from a shortage of memory or a failed system call. */
    if (err_str) {
      if (rv == ccol_not_permitted)
        *err_str = CCOL_ERR_STR(
            "ccol_event_loop_add: direction already in use on this fd");
      else if (rv == ccol_not_enough_memory)
        *err_str = CCOL_ERR_STR(
            "ccol_event_loop_add: failed to allocate the registration");
      else
        *err_str = CCOL_ERR_STR(
            "ccol_event_loop_add: a system call failed (epoll_ctl, eventfd "
            "or a mutex initialisation)");
    }
    /* reg already has a slot. The code acquired it above, before it tried any
     * wiring. But nothing ever linked reg into any registry. Every failure
     * path inside _ccol_event_loop_add_fd and _ccol_event_loop_add_queue
     * leaves *slot or entry.as.reg back at NULL. Each one also undoes its own
     * partial epoll_ctl work before it returns. There is therefore nothing
     * here for the two-phase unlink and finish steps of
     * ccol_event_loop_remove to undo. No dispatch can ever have seen reg.
     *
     * The code therefore releases the slot directly. That mirrors the
     * order of ccol_event_loop_remove: release the slot before reg becomes
     * eligible for reclaim. No other thread can have resolved this handle,
     * because the code never returned h to any caller. The code then frees
     * reg outright. It does not defer reg through the pending-free and epoch
     * machinery. That machinery only exists to protect the in-flight dispatch
     * of a registration that is already live. */
    _ccol_event_reg_slot_release(raw, reg);
    _ccol_event_reg_free(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return CCOL_EVENT_REG_INVALID;
  }

#ifdef RUNNING_UNIT_TESTS
  void (*add_hook)(void) = atomic_load(&g_evl_add_before_return_hook);
  if (add_hook) add_hook();
#endif
  _ccol_event_loop_resolve_unpin(raw);
  if (err_str) *err_str = NULL;
  return h;
}

uint64_t ccol_event_loop_reg_generation(ccol_event_loop loop,
                                        ccol_event_reg reg) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return 0;
  ccol_event_reg_s *raw_reg = _ccol_event_reg_resolve(raw, reg);
  if (!raw_reg) {
    _ccol_event_loop_resolve_unpin(raw);
    return 0;
  }
  uint64_t gen = raw_reg->generation;
  _ccol_event_reg_resolve_unpin(raw, raw_reg);
  _ccol_event_loop_resolve_unpin(raw);
  return gen;
}

/* Computes the epoll interest mask that entry wants now, fresh, and applies
 * it. For an fd it computes that mask from the live state of
 * entry->as.fd.read_reg and entry->as.fd.write_reg. For a queue or
 * ccol_channel it computes it from entry->as.reg. It never uses a cached
 * snapshot from an earlier point in time.
 *
 * In the fd case, every live direction can be paused now. The computed mask
 * then carries no real interest bit at all, beside EPOLLONESHOT when
 * dispatch_pool exists. The kernel reports EPOLLERR and EPOLLHUP
 * unconditionally, whatever the registered mask holds. An empty mask can
 * therefore never silence an fd that is, or becomes, in an error or hangup
 * condition while it is paused. A level-triggered epoll_wait then reports it
 * for ever. The re-check of reg->paused in
 * _ccol_event_loop_run_callback skips the callback itself every time. But
 * nothing stops the reactor from seeing and collecting that fd again on every
 * single epoll_wait call. That is an unbounded CPU-spin busy loop. It also
 * silently contradicts the contract of ccol_event_loop_pause, which
 * promises that no callback fires, exactly as after a remove. A registration
 * that the code really removed produces no further wake, because of
 * EPOLL_CTL_DEL. A registration with only a narrower mask cannot do that,
 * because the mask cannot opt out of ERR and HUP.
 *
 * The code therefore really issues EPOLL_CTL_DEL in that case. It tracks the
 * result in entry->epoll_added. An fd that the code removed this way needs
 * EPOLL_CTL_ADD, and not EPOLL_CTL_MOD, once some direction wants real
 * interest again. MOD on an fd that is not in the interest set now fails with
 * ENOENT. See the field comment of event_entry.epoll_added.
 *
 * Five call sites share this function. The first is the direction flip
 * of ccol_event_loop_modify. The second is the partial-removal branch of
 * ccol_event_loop_remove, where one direction of an fd remains. The third is
 * the mask-exclude step of ccol_event_loop_pause. The fourth is the
 * mask-include step of ccol_event_loop_resume. The fifth is the
 * post-dispatch EPOLLONESHOT re-arm of
 * _ccol_event_loop_dispatch_job_fn. See the comment of
 * _ccol_event_loop_add_fd for the reason why every registration needs
 * EPOLLONESHOT when dispatch_pool exists.
 *
 * One shared implementation is what makes all five call sites race-free
 * against each other. Whichever one runs later under the same stripe lock
 * always computes and applies "what must be armed right now" fresh. That
 * answer now includes the paused state of each fd-direction reg, and not
 * only whether a slot is occupied. Take a concurrent ccol_event_loop_modify
 * and a re-arm by a worker for the other direction of the same fd. Or take a
 * ccol_event_loop_pause that races the post-dispatch re-arm of a dispatch
 * job. Each such pair can only ever be redundant with the other, and never
 * racy. Neither one ever replays a stale mask that the other already moved
 * past.
 *
 * The function does nothing when entry->removed is set. The entry then has no
 * live registration left at all. The code fully removed it while a dispatch
 * job for it was still in flight. The application can also already have
 * closed or reused its fd by now. The generation counter guards callers
 * against the same hazard in other places. This flag applies that guard to
 * the internal re-arm call of this module. The function also does
 * nothing for a queue entry whose entry->as.reg a concurrent
 * ccol_event_loop_remove already set to NULL. See the comment of that
 * function for the reason why it clears this field before it defers the free
 * of the entry.
 *
 * The caller must already hold loop->stripes[entry->stripe_idx].lock. */
static void _ccol_event_loop_rearm_entry_locked(struct ccol_event_loop_s *loop,
                                                event_entry *entry) {
  if (atomic_load(&entry->removed)) return;

  ccol_poll_event ev;
  ev.data.ptr = entry;

  if (entry->is_fd) {
    uint32_t mask = loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0;
    mask |= _fd_reg_interest_bits(entry->as.fd.read_reg, ccol_select_read);
    mask |= _fd_reg_interest_bits(entry->as.fd.write_reg, ccol_select_write);

    /* EPOLLONESHOT alone, or a literal 0, is not real interest. Every live
     * direction is paused now. The code therefore really drops the fd from
     * the interest set. It does not hand the kernel a hollow mask, because
     * such a mask still cannot suppress the unconditional EPOLLERR and
     * EPOLLHUP monitoring. See the doc comment of this function. */
    bool wants_interest = (mask & ~(uint32_t)CCOL_POLL_ONESHOT) != 0;
    if (wants_interest) {
      ev.events = mask;
      int op = entry->epoll_added ? CCOL_POLL_CTL_MOD : CCOL_POLL_CTL_ADD;
      if (ccol_poll_ctl(loop->epfd, op, entry->fd, &ev) == 0)
        entry->epoll_added = true;
    } else if (entry->epoll_added) {
      ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_DEL, entry->fd, NULL);
      entry->epoll_added = false;
    }
  } else if (entry->as.reg) {
    ev.events = CCOL_POLL_IN | (loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0);
    ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_MOD, entry->as.reg->bridge_efd,
                  &ev);
  }
}

/* This function deliberately does not also take entry->dispatch_lock. Such a
 * lock needs a read of reg->owning_entry before the stripe lock confirms that
 * reg->removed is false. That is exactly the unsafe read pattern that the doc
 * comment of owning_entry warns against. See the stripe_idx field of
 * ccol_event_reg. The code only ever takes entry->dispatch_lock after that
 * stripe-lock-protected confirmation, in _ccol_event_loop_handle_event, and
 * never before it.
 *
 * The write of this function to reg->sel.dir below can therefore genuinely
 * run at the same time as the use of *sel by an in-flight callback. That is a
 * real race. It is independent of the dispatch_lock of this module, which
 * neither adds it nor closes it. See the comment of
 * _dispatch_item.sel_snapshot for what makes it safe instead. The dispatch
 * path snapshots reg->sel under the stripe lock at collection time. It never
 * hands a callback a live pointer into reg->sel. The write here also happens
 * under the stripe lock. It can therefore never race a read by a callback. */
ccol_retval_t ccol_event_loop_modify(ccol_event_loop loop, ccol_event_reg reg_h,
                                     ccol_select_dir new_dir) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  if (new_dir != ccol_select_read && new_dir != ccol_select_write) {
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  /* A resolve of reg_h through the reg slot table of loop is what makes
   * every field read below safe. The code detects a stale or already-removed
   * handle right here, before it dereferences anything. It never learns that
   * by a touch of memory that can already be freed. See the reg_slots
   * field comment of struct ccol_event_loop_s. */
  ccol_event_reg_s *reg = _ccol_event_reg_resolve(raw, reg_h);
  if (!reg) {
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (reg->sel.type != ccol_selectable_fd) {
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol_event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  ccol_mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    ccol_mutex_unlock(stripe->lock);
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (reg->sel.dir == new_dir) {
    /* The direction stays. A muted reg is still watched again, as the
     * documentation of this function promises for every successful call.
     * The confirmed removed == false makes owning_entry safe to read. */
    if (reg->muted) {
      reg->muted = false;
      _ccol_event_loop_rearm_entry_locked(raw, reg->owning_entry);
    }
    ccol_mutex_unlock(stripe->lock);
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_success;
  }

  /* The check above just confirmed that removed is false. It did that under
   * the lock of this exact stripe, which is the only lock under which removed
   * can become true. owning_entry is therefore guaranteed to be not yet freed
   * here. */
  event_entry *entry = reg->owning_entry;
  ccol_event_reg_s **target_slot = (new_dir == ccol_select_read)
                                       ? &entry->as.fd.read_reg
                                       : &entry->as.fd.write_reg;
  if (*target_slot != NULL) {
    ccol_mutex_unlock(stripe->lock);
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_not_permitted;
  }

  ccol_event_reg_s **current_slot = (reg->sel.dir == ccol_select_read)
                                        ? &entry->as.fd.read_reg
                                        : &entry->as.fd.write_reg;
  *current_slot = NULL;
  *target_slot = reg;
  reg->sel.dir = new_dir;
  reg->muted = false;

  _ccol_event_loop_rearm_entry_locked(raw, entry);

  ccol_mutex_unlock(stripe->lock);
  _ccol_event_reg_resolve_unpin(raw, reg);
  _ccol_event_loop_resolve_unpin(raw);
  return ccol_success;
}

/* Shared validation for ccol_event_loop_pause and ccol_event_loop_resume.
 * Both are for an fd only. ccol_event_loop_modify carries the same
 * restriction. The bridge eventfd of a queue or ccol_channel registration has
 * no equivalent use case of "stop caring for a while, but keep the
 * registration". Nothing outside this module ever touches the fd of a
 * queue directly. chttpserver and chttpclient do read and write the fd of a
 * connection themselves during a paused window.
 *
 * Both functions must also confirm reg->removed again under the stripe
 * lock of reg, before they touch owning_entry. ccol_event_loop_modify already
 * does this for the identical reason. See the comment of that function on
 * reg->stripe_idx.
 *
 * The function returns ccol_success with *out_entry set to reg->owning_entry
 * when the caller must go on. Any other return value means that the caller
 * must unlock and return that value unchanged. */
static ccol_retval_t _ccol_event_loop_pause_resume_validate_locked(
    struct ccol_event_loop_s *loop, ccol_event_reg_s *reg,
    event_entry **out_entry) {
  (void)loop;
  if (reg->sel.type != ccol_selectable_fd) return ccol_invalid_args;
  if (atomic_load(&reg->removed)) return ccol_invalid_args;
  *out_entry = reg->owning_entry;
  return ccol_success;
}

/* @brief Temporarily stop delivering events for an fd registration, without
 * destroying it.
 *
 * ccol_event_loop_remove is different. It fully unregisters the registration
 * and defers its free. ccol_event_loop_pause leaves reg fully intact. reg
 * keeps its slot on the entry of the fd below it. It still counts toward
 * ccol_event_loop_reg_count. It keeps the same ccol_event_loop_reg_generation.
 * The function only computes the combined epoll interest mask of the fd again
 * and leaves reg out of it.
 *
 * This is the cheap alternative to a ccol_event_loop_remove with a later
 * ccol_event_loop_add. It fits a caller pattern where the same logical
 * registration comes back. One example is a connection that goes to a worker
 * thread for blocking body I/O. That connection then goes back to the
 * reactor for its next request. The pause costs no heap allocation and no
 * free. It also causes no churn in the fd-registry chmap. It costs one
 * epoll_ctl call. A remove and add pair costs two, which are a DEL and then
 * an ADD.
 *
 * While reg is paused, no on_readable, on_writable or on_error callback fires
 * for it. That is exactly the same as after a remove. The other direction on
 * the same fd, when there is one, is not affected. A pause of a reg that is
 * already paused is a success and does nothing.
 *
 * @param loop The ccol_event_loop that the registration belongs to
 * @param reg  The registration to pause
 *
 * @return ccol_success on success
 * @return ccol_invalid_args when loop or reg is NULL. It also comes back
 * when reg is a queue or ccol_channel registration, and when another thread
 * removed reg at the same time.
 *
 * @note Thread-safe. A caller can call it at the same time as
 *       ccol_event_loop_remove. It can also call it from inside a callback
 *       that runs on the reactor thread.
 *
 * @see ccol_event_loop_resume
 */
ccol_retval_t ccol_event_loop_pause(ccol_event_loop loop,
                                    ccol_event_reg reg_h) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;

  ccol_event_reg_s *reg = _ccol_event_reg_resolve(raw, reg_h);
  if (!reg) {
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol_event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  ccol_mutex_lock(stripe->lock);

  event_entry *entry = NULL;
  ccol_retval_t rv =
      _ccol_event_loop_pause_resume_validate_locked(raw, reg, &entry);
  if (rv != ccol_success) {
    ccol_mutex_unlock(stripe->lock);
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return rv;
  }

  if (!atomic_load(&reg->paused)) {
    atomic_store(&reg->paused, true);
    _ccol_event_loop_rearm_entry_locked(raw, entry);
  }

  ccol_mutex_unlock(stripe->lock);
  _ccol_event_reg_resolve_unpin(raw, reg);
  _ccol_event_loop_resolve_unpin(raw);
  return ccol_success;
}

/* @brief Resume event delivery for a registration previously paused by
 * ccol_event_loop_pause.
 *
 * It computes the combined epoll interest mask of the fd again and includes
 * reg in it. A resume of a reg that is not paused now is a success and does
 * nothing. Such a reg was never paused, or the code already resumed it. This
 * deliberately mirrors the "already in the requested state" behaviour of
 * ccol_event_loop_modify. An error would be worse. A caller can race its own
 * pause and resume pair against a concurrent ccol_event_loop_remove. It must
 * not have to tell "already resumed" from "nothing to do" from the return
 * value alone.
 *
 * @param loop The ccol_event_loop that the registration belongs to
 * @param reg  The registration to resume
 *
 * @return ccol_success on success
 * @return ccol_invalid_args when loop or reg is NULL. It also comes back
 * when reg is a queue or ccol_channel registration, and when another thread
 * removed reg at the same time. For example, the connection closes while the
 * caller still thinks that it owns a paused registration to resume.
 *
 * @note Thread-safe. A caller can call it at the same time as
 *       ccol_event_loop_remove. It can also call it from inside a callback
 *       that runs on the reactor thread.
 *
 * @see ccol_event_loop_pause
 */
ccol_retval_t ccol_event_loop_resume(ccol_event_loop loop,
                                     ccol_event_reg reg_h) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;

  ccol_event_reg_s *reg = _ccol_event_reg_resolve(raw, reg_h);
  if (!reg) {
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol_event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  ccol_mutex_lock(stripe->lock);

  event_entry *entry = NULL;
  ccol_retval_t rv =
      _ccol_event_loop_pause_resume_validate_locked(raw, reg, &entry);
  if (rv != ccol_success) {
    ccol_mutex_unlock(stripe->lock);
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return rv;
  }

  if (atomic_load(&reg->paused) || reg->muted) {
    atomic_store(&reg->paused, false);
    reg->muted = false;
    _ccol_event_loop_rearm_entry_locked(raw, entry);
  }

  ccol_mutex_unlock(stripe->lock);
  _ccol_event_reg_resolve_unpin(raw, reg);
  _ccol_event_loop_resolve_unpin(raw);
  return ccol_success;
}

/* ccol_event_loop_remove cannot free an event_entry synchronously. The
 * epoll_ctl(DEL) call and the clear of the entry slot already stop any FUTURE
 * epoll_wait call from a new event for it. That is still not enough.
 * epoll_wait can return a batch of several ready events in one call. A
 * reactor thread then processes them one at a time. The event of this entry
 * can sit at a later index of a batch that the code already fetched. That
 * fetch happened before this remove() call, and the batch sits in the local
 * stack array of that thread. A free of the entry here races that
 * not-yet-processed index against ccol_event_loop_remove. That is a genuine
 * use-after-free: a read of a stale event_entry* after a concurrent remove(),
 * which SIGSEGVs. epoll_ctl(DEL) cannot invalidate an event that the kernel
 * already copied out into userspace.
 *
 * The code therefore defers the real free. It defers it to a point where
 * poller_thread can prove that no batch can still reference this entry. That
 * point is between the end of one epoll_wait batch and the start of the next.
 * That closes the race. With a dispatch pool, which means
 * num_reactor_threads > 1, that batch boundary is not enough on its own. A
 * ctpool worker can still be in the middle of a dispatch of a job that
 * references this entry. That is independent of how far poller_thread moved
 * on. See the large comment above _ccol_event_loop_reclaim_pending_frees for
 * the epoch-and-refcount scheme that this module uses to cover both. This
 * function only pushes the node with its defer_gen snapshot stamped on it. It
 * does not decide when a free is really safe.
 *
 * The push is a lock-free Treiber-stack push. loop->pending_entry_frees is
 * loop-wide aggregate state, and not per-stripe state. Entries from every
 * stripe go onto this one list, so one stripe lock could not protect it. The
 * push carries no ABA hazard. A node only ever comes off the list as part of
 * a claim of the WHOLE list at once, with one atomic_exchange. See
 * _ccol_event_loop_reclaim_pending_frees. The code never pops and frees one
 * node at a time while the list is still shared. */
static void _ccol_event_loop_push_entry_free_node(
    struct ccol_event_loop_s *loop, event_entry *entry) {
  event_entry *old_head = atomic_load(&loop->pending_entry_frees);
  do {
    entry->pending_free_next = old_head;
  } while (!atomic_compare_exchange_weak(&loop->pending_entry_frees, &old_head,
                                         entry));
}

static void _ccol_event_loop_defer_entry_free(struct ccol_event_loop_s *loop,
                                              event_entry *entry) {
  atomic_store(&entry->removed, true);
  entry->defer_gen = atomic_load(&loop->poller_batch_gen);
  _ccol_event_loop_push_entry_free_node(loop, entry);
}

/* A ccol_event_reg also cannot be freed synchronously. The reason is related
 * to the one in _ccol_event_loop_defer_entry_free, but it is not the same. A
 * dispatch callback can still be in flight, which means a refcount above 1 at
 * the moment of the removal. That callback can still need reg after
 * ccol_event_loop_remove returns. The code therefore defers the free until
 * the refcount genuinely reaches 0. The caller checks that before it ever
 * calls this function. The memory stays valid, so that callback can finish
 * safely.
 *
 * Once the code defers a reg, the real reclaim also waits for
 * reg->pending_resolve_count to reach 0. See the reg_slots field comment
 * of struct ccol_event_loop_s and _ccol_event_loop_reclaim_pending_frees.
 * That wait is what lets the documentation promise a graceful
 * ccol_invalid_args, instead of undefined behaviour. The promise covers the
 * case where ccol_event_loop_remove and ccol_event_loop_modify race each
 * other on the same reg. The list has the same lock-free Treiber-stack shape
 * as
 * _ccol_event_loop_push_entry_free_node and
 * _ccol_event_loop_defer_entry_free. */
static void _ccol_event_loop_push_reg_free_node(struct ccol_event_loop_s *loop,
                                                ccol_event_reg_s *reg) {
  ccol_event_reg_s *old_head = atomic_load(&loop->pending_reg_frees);
  do {
    reg->pending_free_next = old_head;
  } while (
      !atomic_compare_exchange_weak(&loop->pending_reg_frees, &old_head, reg));
}

static void _ccol_event_loop_defer_reg_free(struct ccol_event_loop_s *loop,
                                            ccol_event_reg_s *reg) {
  _ccol_event_loop_push_reg_free_node(loop, reg);
}

/* Reclaims every entry/reg deferred by _ccol_event_loop_defer_entry_free /
 * _ccol_event_loop_defer_reg_free that is now provably safe to free, and
 * otherwise leaves it pending for a later attempt.
 *
 * Exactly one thread, poller_thread, ever calls epoll_wait, for every
 * configuration. "Safe to free" therefore needs one scalar comparison.
 * poller_thread calls this function at its own between-batches point. See
 * _ccol_event_loop_thread_fn. By then it already published its own advanced
 * poller_batch_gen. An item is eligible once its defer_gen snapshot is older
 * than the current poller_batch_gen. That means that poller_thread crossed a
 * between-batches point at least once after the deferral. It can therefore no
 * longer be in the middle of whatever batch it held in flight at defer time.
 * The code also removed the item from the registry before it deferred it,
 * with an epoll_ctl(DEL) or a clear of its slot. No FUTURE batch can
 * reference it either.
 *
 * An entry needs one more condition than the epoch condition: refcount == 0.
 * See the comment of event_entry.refcount. With num_reactor_threads > 1,
 * the dispatch job of a ctpool worker can still use an entry well after
 * poller_thread moves past the epoch of its deferral. The epoch check on its
 * own says nothing about that. A reg needs no such extra check here. Its own
 * refcount mechanism already stops the code from ever deferring it while a
 * callback still needs it. That callback can run inline or on a worker. See
 * _ccol_event_loop_release_after_dispatch.
 *
 * The function is lock-free. It claims the whole contents of each list in one
 * atomic_exchange. It then sorts the items outside any lock into "free now"
 * and "not eligible yet". It pushes each item that is not eligible yet back
 * with the same CAS-based push that the original defer call used. It does NOT
 * stamp defer_gen again. The original snapshot of those items is what lets
 * them make forward progress. A new stamp resets their eligibility clock
 * every time a reclaim attempt finds them still pending. Under sustained load
 * that can starve them for ever.
 *
 * __ccol_event_loop_destroy also uses this logic, in its unconditional form.
 * See _ccol_event_loop_free_all_pending. It uses it after it fully joined
 * poller_thread and drained dispatch_pool, when there is one. No epoch check
 * and no refcount check is needed there, because no thread can reference
 * anything any more.
 *
 * The function returns true when anything at all stays pending. That means
 * that the code pushed it back onto either list instead of a free. The
 * function _ccol_event_loop_thread_fn uses that answer to choose its NEXT
 * epoll_wait timeout. A reg that is still pending and that carries a
 * non-NULL on_removed handler needs a bounded retry, and not an indefinite
 * one. The reason is that no ping is reliable. Whichever call finally drops
 * its pending_resolve_count to 0 emits that ping, and it can fail to fire.
 * See the comment of _ccol_event_reg_resolve_unpin for the reason why
 * that snapshot can go stale against a concurrent ccol_event_loop_remove.
 * That comment also explains what a read of the fields of raw AFTER the
 * reclaim of this function would do. It would only open the use-after-free
 * that this epoch and refcount scheme exists to prevent.
 * A bounded retry here is therefore the correctness backstop. The pings stay
 * as a latency optimization for the common case, where they do fire
 * correctly. */
static bool _ccol_event_loop_reclaim_pending_frees(
    struct ccol_event_loop_s *loop) {
  uint64_t reached = atomic_load(&loop->poller_batch_gen);

  event_entry *e = atomic_exchange(&loop->pending_entry_frees, NULL);
  event_entry *e_keep = NULL;
  while (e) {
    event_entry *next = e->pending_free_next;
    if (e->defer_gen < reached && atomic_load(&e->refcount) == 0) {
      ccol_mutex_destroy(e->dispatch_lock);
      _ccol_mem_free(loop->m_procs, e);
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add_explicit(&loop->entries_reclaimed_for_tests, (uint64_t)1,
                                memory_order_relaxed);
#endif
    } else {
      e->pending_free_next = e_keep;
      e_keep = e;
    }
    e = next;
  }
  bool entries_still_pending = (e_keep != NULL);
  while (e_keep) {
    event_entry *next = e_keep->pending_free_next;
    _ccol_event_loop_push_entry_free_node(loop, e_keep);
    e_keep = next;
  }

  /* The eligibility condition of a reg is pending_resolve_count == 0, and
   * not an epoch comparison. See the reg_slots field comment of struct
   * ccol_event_loop_s. The code already released the slot of that reg, when
   * it had one. It released it under the write side of loop->reg_slot_rwlock,
   * at the exact point of the deferral. See the two-phase call site of
   * ccol_event_loop_remove. No NEW resolve of it can succeed from that point
   * on. A pending_resolve_count of 0 therefore means that no thread anywhere
   * still holds a pointer to this reg from an in-flight call. Such a call is
   * a ccol_event_loop_modify, _pause, _resume, _remove or
   * ccol_event_loop_reg_generation. The code can therefore free the reg
   * outright, with no further lock. */
  ccol_event_reg_s *r = atomic_exchange(&loop->pending_reg_frees, NULL);
  ccol_event_reg_s *r_keep = NULL;
  while (r) {
    ccol_event_reg_s *next = r->pending_free_next;
    if (atomic_load(&r->pending_resolve_count) == 0) {
      _ccol_event_reg_free(loop, r);
    } else {
      r->pending_free_next = r_keep;
      r_keep = r;
    }
    r = next;
  }
  bool regs_still_pending = (r_keep != NULL);
  while (r_keep) {
    ccol_event_reg_s *next = r_keep->pending_free_next;
    _ccol_event_loop_push_reg_free_node(loop, r_keep);
    r_keep = next;
  }

  return entries_still_pending || regs_still_pending;
}

/* The unconditional variant. The code uses it after it joined every reactor
 * thread, which means after ccol_event_loop_shutdown returned. No thread
 * exists any more that can reference anything. Every deferred item that
 * remains is therefore safe to free, whatever its defer_gen holds. This
 * function frees whatever the very last call of
 * _ccol_event_loop_reclaim_pending_frees before the join left pending. */
static void _ccol_event_loop_free_all_pending(struct ccol_event_loop_s *loop) {
  event_entry *e = atomic_exchange(&loop->pending_entry_frees, NULL);
  while (e) {
    event_entry *next = e->pending_free_next;
    ccol_mutex_destroy(e->dispatch_lock);
    _ccol_mem_free(loop->m_procs, e);
    e = next;
  }
  ccol_event_reg_s *r = atomic_exchange(&loop->pending_reg_frees, NULL);
  while (r) {
    ccol_event_reg_s *next = r->pending_free_next;
    _ccol_event_reg_free(loop, r);
    r = next;
  }
}

/* Phase 1 of the shared core of ccol_event_loop_remove. It takes an
 * already-resolved raw reg pointer directly, and not a handle. The public
 * ccol_event_loop_remove uses it, after it resolves and pins reg with
 * _ccol_event_reg_resolve. ccol_event_loop_add has no use for it. That
 * function acquires the slot of reg BEFORE it wires reg into the fd or
 * queue registry. See its own comment on _ccol_event_reg_slot_acquire. A
 * wiring failure there therefore always finds reg still fully unlinked, and
 * the unlink logic of this function has nothing to do.
 * ccol_event_loop_add releases the slot and frees reg directly in that case.
 *
 * This function unlinks reg from the fd or queue registry and marks it
 * removed. It does NOT touch the refcount, and it does not defer reg for a
 * reclaim yet. See _ccol_event_loop_remove_finish, which is phase 2, and the
 * call site of ccol_event_loop_remove for the reason why that split
 * matters. The slot of reg, when it has one, must be released BETWEEN the
 * two phases. It must be released before the code pushes reg onto
 * loop->pending_reg_frees. Otherwise a second resolve that races the slot
 * release can still succeed against a reg that is already eligible for a real
 * reclaim.
 *
 * The function returns true when this call is the one that really did the
 * removal. In that case reg->removed was false and is now true. It returns
 * false when an earlier call already removed reg. That is a graceful no-op.
 * The caller must then not go on to phase 2 and must not release a slot. */
static bool _ccol_event_loop_remove_unlink(struct ccol_event_loop_s *raw,
                                           ccol_event_reg_s *reg) {
  ccol_event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  ccol_mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    ccol_mutex_unlock(stripe->lock);
    return false;
  }

  event_entry *entry = reg->owning_entry;

  if (reg->sel.type == ccol_selectable_fd) {
    ccol_event_reg_s **slot = (reg->sel.dir == ccol_select_read)
                                  ? &entry->as.fd.read_reg
                                  : &entry->as.fd.write_reg;
    *slot = NULL;
    if (entry->as.fd.read_reg == NULL && entry->as.fd.write_reg == NULL) {
      /* entry->epoll_added can already be false here. That happens when
       * every direction was paused, because
       * _ccol_event_loop_rearm_entry_locked then already issued its own
       * EPOLL_CTL_DEL. See the comment of that function. The code
       * therefore issues a second one only when the fd is still
       * registered. */
      if (entry->epoll_added)
        ccol_poll_ctl(raw->epfd, CCOL_POLL_CTL_DEL, entry->fd, NULL);
      _fd_registry_remove(stripe, entry->fd);
      _ccol_event_loop_defer_entry_free(raw, entry);
    } else {
      _ccol_event_loop_rearm_entry_locked(raw, entry);
    }
  } else {
    ccol_mutex_t *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);

    /* The splice and the removed=true store MUST happen under ONE SINGLE
     * q_mtx critical section. The splice unlinks reg from the waiter list
     * of the queue. The code must not use two separate lock and unlock pairs
     * on the same mutex. Once the splice runs, reg->waiter_node.next and
     * .prev are frozen. A splice only updates the pointers of the NEIGHBOURS
     * of reg, and never the pointers of reg. reg is no longer linked for
     * any future splice to touch. See _sel_unlink_waiter_locked.
     *
     * Take a design that sets removed to true only in a later, separate
     * critical section. A concurrent call of
     * _ccol_event_loop_queue_cascade_notify_next(reg) can then lock q_mtx in
     * the gap between the two. That call runs on another thread. It is still
     * in the middle of the dispatch of the already-collected callback of
     * reg, because ccol_event_loop_remove does not wait for that to finish.
     * It observes removed as still false, and it reads reg->waiter_node.next.
     * That value is already stale. It is frozen at whichever registration was
     * the successor of reg at splice time. Now take a THIRD, independent
     * ccol_event_loop_remove() call on that successor. It can complete and
     * reclaim the successor before cascade-notify gets there. The reclaim of
     * a ccol_event_reg has no epoch delay once its refcount and its
     * pending_resolve_count both reach 0. Cascade-notify then calls
     * _notify_waiter() on memory that is already freed.
     *
     * One critical section for both mutations closes this. By the time that
     * the lock(q_mtx) of cascade-notify succeeds, there are two possible
     * states. Both the splice and removed=true already happened together, so
     * removed reads true and nothing touches .next. Or neither happened, so
     * the queue, and every registration that is still linked to it, is fully
     * intact.
     *
     * This is what makes the documented pattern of this module safe for a
     * QUEUE selectable too. That pattern is: call ccol_event_loop_remove, and
     * then free or destroy the resource below it at once. fd selectables
     * already depend on it, through the removed re-check of
     * _ccol_event_loop_run_callback. The function
     * _ccol_event_loop_queue_cascade_notify_next also touches this same queue
     * after a dispatch. The critical
     * section of that function locks q_mtx, checks removed, perhaps notifies,
     * and unlocks q_mtx. Mutexes guarantee that it can only fully precede or
     * fully follow this one, and never overlap it. When it precedes, the
     * queue is obviously still alive, and so is every registration that the
     * .next of reg can point to. When it follows, removed is already
     * visible as true the moment that its own lock succeeds. A mutex release
     * and acquire is itself a memory barrier. It then never touches
     * reg->waiter_node.next at all.
     *
     * This combination is load-bearing and not defensive. A split of the
     * splice and the removed=true store into two separate critical sections
     * reliably reproduces a heap-use-after-free under valgrind. The report is
     * a ccol_mutex_lock on the mutex of a queue that the code already
     * destroyed. The test pattern is: wait for the signal of the
     * callback, then remove and destroy the queue at once from a different
     * thread.
     *
     * The same critical section hands on the wake that reg can still hold.
     * A send or a receive wakes exactly one listener. When that listener is
     * reg, and reg leaves before a dispatch acted on the wake, the dispatch
     * that follows sees removed and does nothing, so the message would sit in
     * the queue with every other listener asleep until some later send. The
     * wake therefore goes to the next listener whenever the queue is still
     * ready for the direction of reg, exactly as a departing ccol_select()
     * waiter does in deregister_sel_waiter. A wake that reg did not hold
     * costs one spurious dispatch of a listener that finds the queue ready
     * in any case. */
    ccol_mutex_lock(*q_mtx);
    _sel_unlink_waiter_locked(&reg->waiter_node, q_head, q_rotor);
    atomic_store(&reg->removed, true);
    if (_queue_sel_is_ready(&reg->sel)) notify_one_sel_waiter(q_head, q_rotor);
    ccol_mutex_unlock(*q_mtx);

    ccol_poll_ctl(raw->epfd, CCOL_POLL_CTL_DEL, reg->bridge_efd, NULL);
    _loop_queue_list_remove(stripe, reg);
    /* The code defers entry itself below, and that memory stays safe and
     * valid. But its as.reg field must become NULL HERE, under stripe->lock,
     * before that deferral. Without it, a stale batch entry can read
     * entry->as.reg after the code deferred and freed reg. It then gets a
     * pointer that looks live into the pending-free list, instead of a clean
     * NULL. The fd branch above does the equivalent with *slot = NULL. */
    entry->as.reg = NULL;
    _ccol_event_loop_defer_entry_free(raw, entry);
  }

  if (reg->sel.type == ccol_selectable_fd) {
    atomic_store(&reg->removed, true);
  }

  ccol_mutex_unlock(stripe->lock);
  return true;
}

/* Phase 2. It decrements the refcount. When that was the last reference, it
 * defers reg for a reclaim. It must run strictly AFTER the code released the
 * slot of reg, when reg has one. reg is then never both on the
 * pending-free list and still resolvable through its old handle. See the
 * comment of _ccol_event_loop_remove_unlink for the reason. */
static void _ccol_event_loop_remove_finish(struct ccol_event_loop_s *raw,
                                           ccol_event_reg_s *reg) {
  atomic_fetch_sub(&raw->reg_count, 1);

  int prev = atomic_fetch_sub(&reg->refcount, 1);
  /* The code defers reg and does not free it here directly. A callback can
   * still be in flight, and it can still need reg. See the comment of
   * _ccol_event_loop_defer_reg_free and the one of
   * _ccol_event_loop_reclaim_pending_frees for the full reclaim scheme. That
   * scheme waits for reg->pending_resolve_count to reach 0. See the
   * reg_slots field comment of struct ccol_event_loop_s. */
  if (prev == 1) _ccol_event_loop_defer_reg_free(raw, reg);

  /* The code pings the reclaim_wake_efd of poller_thread
   * unconditionally. See the field comment of that fd. By this point the
   * removal ALWAYS deferred something. _ccol_event_loop_remove_unlink
   * deferred the event_entry of reg. Both of its branches do that. The
   * one exception is the fd case where the other direction is still
   * registered, which leaves the entry live on purpose. The refcount drop
   * above can also have deferred reg itself.
   *
   * Only poller_thread ever reclaims deferred memory, from its own
   * between-batches point. Neither an epoll_ctl(EPOLL_CTL_DEL) nor an
   * epoll_ctl(EPOLL_CTL_ADD) of an fd that is not ready now wakes a blocked
   * epoll_wait. On a loop that is otherwise idle, nothing else brings
   * poller_thread back at all. The deferred entry and reg then sit on
   * loop->pending_entry_frees and pending_reg_frees indefinitely. They hold
   * reg->bridge_efd open, and only _ccol_event_reg_free closes it. An add and
   * remove cycle that repeats on an idle loop turns that into an unbounded
   * file descriptor leak.
   *
   * The bounded EVENT_LOOP_RECLAIM_RETRY_MS retry is no substitute. It only
   * engages once poller_thread already woke at least once after the deferral
   * and saw something still pending. This ping is also what gives an
   * on_removed handler its small, bounded latency. When a dispatch is still
   * in flight, which means prev != 1, poller_thread is already running by
   * construction. The ping is then a harmless, cheap no-op that it drains on
   * its very next return from epoll_wait. */
  _eventfd_notify(raw->reclaim_wake_efd);
}

ccol_retval_t ccol_event_loop_remove(ccol_event_loop loop,
                                     ccol_event_reg reg_h) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  ccol_event_reg_s *reg = _ccol_event_reg_resolve(raw, reg_h);
  if (!reg) {
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (_ccol_event_loop_remove_unlink(raw, reg)) {
    /* This invalidates the handle of this reg at once, for every future
     * resolve attempt. It does that BEFORE phase 2 below can make reg
     * eligible for a real reclaim. See the comment of
     * _ccol_event_loop_remove_unlink for the reason why this order is the
     * race-free one. A release of the slot after phase 2 is not. The code
     * skips this step when this call saw that an earlier call already removed
     * reg, because that call already did it. */
    _ccol_event_reg_slot_release(raw, reg);
    _ccol_event_loop_remove_finish(raw, reg);
  }

  /* This is safe although the code can just have deferred reg for a reclaim.
   * The still-held pin of this call, from the successful resolve above,
   * is exactly what guarantees that
   * _ccol_event_loop_reclaim_pending_frees cannot have freed it yet. */
  _ccol_event_reg_resolve_unpin(raw, reg);
  _ccol_event_loop_resolve_unpin(raw);
  return ccol_success;
}

size_t ccol_event_loop_reg_count(ccol_event_loop loop) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_size;
  size_t n = atomic_load(&raw->reg_count);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}

/* One reg collected for dispatch under the entry's stripe lock, acted on
 * after that lock is released. */
typedef struct _dispatch_item {
  ccol_event_reg_s *reg;
  bool is_error;
  bool is_readable;
  bool is_writable;
  /* A snapshot of reg->sel. The code takes it under the stripe lock at
   * collection time. See _ccol_event_loop_handle_event. It is not a live
   * pointer into reg->sel itself. ccol_event_loop_modify updates reg->sel.dir
   * under the stripe lock only. See the comment of that function for the
   * reason why it cannot also take entry->dispatch_lock. Such a lock
   * conflicts with the lock order of the safe read of owning_entry.
   *
   * The code can hand a callback a live `&reg->sel` pointer AFTER it
   * releases the stripe lock. Such a pointer is a genuine, unsynchronized
   * concurrent read and write on reg->sel.dir. It sits between a callback in
   * flight and a concurrent ccol_event_loop_modify call from any other
   * thread. ThreadSanitizer reports it. It is reachable even with a single
   * reactor thread, because the documentation lets any thread call
   * ccol_event_loop_modify at the same time as a dispatch.
   *
   * One snapshot, copied under the stripe lock beside the rest of the
   * collection, is what makes that race impossible. The callback sees a
   * self-consistent, well-defined selectable, as of collection time. The
   * documented contract of ccol_event_readable_fn and
   * ccol_event_writable_fn still holds: sel->dir shows the current direction
   * of the registration, and ccol_event_loop_modify can have changed it. The
   * collection reads reg->sel fresh on every single dispatch. A
   * ccol_event_loop_modify call that finished before this collection is
   * therefore still seen correctly. A modify that runs fully at the same time
   * as a callback in flight resolves deterministically to whichever direction
   * was current at collection time. */
  ccol_selectable sel_snapshot;
  /* For a queue-backed item only: the message count of the queue when the
   * dispatch consumed the wake, right before the callback. See
   * _queue_reg_consume_wake and _ccol_event_loop_queue_cascade_notify_next. */
  size_t queue_count_before;
  /* For a queue-backed item only: the hop count of the wake that the
   * dispatch consumed. See the fwd_hops field of ccol_sel_waiter. */
  uint32_t wake_hops;
} _dispatch_item;

/* The two collection paths of this file, _ccol_event_loop_handle_event and
 * _ccol_event_loop_poller_collect, are deliberately separate. One serves
 * num_reactor_threads == 1 and takes entry->dispatch_lock; the other serves
 * the dispatch-pool path and must stay free of every dispatch-pool concern.
 * See the design comment above _ccol_event_loop_poller_collect. That split is
 * about LOCKING and about keeping the one-thread path unburdened.
 *
 * The RULE that decides what an epoll event means for one registration is not
 * part of that split. It is the same rule on both paths, and a version of it
 * that drifted between them would make the reactor dispatch a different
 * callback depending on how many threads the caller configured. The two
 * helpers below hold that rule once, so the two paths cannot disagree about
 * it while they keep their separate locking.
 *
 * Both are static inline and take only values the caller already has. The
 * decision of _ccol_fd_reg_takes_event goes straight into _ccol_fd_item_fill,
 * so neither path computes it twice, and everything past plain readiness of
 * a direction with a handler (an error, a hang-up, and the mute of a
 * registration that nothing can report to) is out of line and cold. */

/* Whether an event on one direction of an fd entry has anything to dispatch
 * to reg at all. The caller holds the stripe lock of the entry.
 *
 * A removed registration is skipped: see the field comment of
 * ccol_event_reg_s.removed. Otherwise, in this order:
 *
 * - The direction is ready and the registration has a handler for that
 *   direction: the event goes to that handler, even when the fd also carries
 *   an error, so that bytes that arrived before the error are not lost.
 * - The registration has on_error: the event goes to on_error. That includes
 *   a ready direction with no handler of its own, which for a read
 *   registration means the peer hung up.
 * - The fd carries an error or a hang-up and the registration has a handler
 *   for its direction but no on_error: the event goes to that handler, as
 *   libevent and libuv do. Its read() or write() returns the error, or 0 for
 *   an end of file, and a read or a write of a socket clears a pending
 *   socket error (SO_ERROR), so a transient error such as the ICMP port
 *   unreachable of a connected UDP socket does not end the delivery of the
 *   data that follows it.
 *
 * An event that no handler of the registration can take is reported to
 * nobody. The condition behind it can stay true, and level-triggered epoll
 * would then report it again on every epoll_wait. The function therefore
 * mutes reg; see _ccol_fd_reg_mute_locked. The test for a handler of the
 * direction comes first, so an ordinary readable or writable event pays
 * nothing for this. */
static void _ccol_fd_reg_mute_locked(struct ccol_event_loop_s *loop,
                                     event_entry *entry, ccol_event_reg_s *reg);

typedef enum {
  _FD_TAKE_NONE,      /* Nothing to dispatch to this registration. */
  _FD_TAKE_DIRECTION, /* Dispatch to the handler of its direction. */
  _FD_TAKE_ERROR,     /* Dispatch to on_error. */
} _fd_take;

static _fd_take _ccol_fd_reg_takes_condition(struct ccol_event_loop_s *loop,
                                             event_entry *entry,
                                             ccol_event_reg_s *reg,
                                             bool is_write_direction);

static inline _fd_take _ccol_fd_reg_takes_event(
    struct ccol_event_loop_s *loop, event_entry *entry, ccol_event_reg_s *reg,
    bool is_err, bool direction_ready, bool is_write_direction) {
  if (!reg || atomic_load(&reg->removed) || !(is_err || direction_ready))
    return _FD_TAKE_NONE;
  if (direction_ready &&
      (is_write_direction ? reg->handlers.on_writable != NULL
                          : reg->handlers.on_readable != NULL))
    return _FD_TAKE_DIRECTION;
  return _ccol_fd_reg_takes_condition(loop, entry, reg, is_write_direction);
}

/* The rest of the rule of _ccol_fd_reg_takes_event, for an event that is not
 * plain readiness of a direction with a handler: an error, a hang-up, or a
 * ready direction with no handler of its own (a peer that hung up, for a
 * read registration). The caller holds the stripe lock of entry. It stays out
 * of line and cold, so the ordinary readable or writable event keeps the
 * short inlined path above. */
static __attribute__((noinline, cold)) _fd_take
_ccol_fd_reg_takes_condition(struct ccol_event_loop_s *loop, event_entry *entry,
                             ccol_event_reg_s *reg, bool is_write_direction) {
  if (reg->handlers.on_error) return _FD_TAKE_ERROR;
  /* No on_error. The caller admitted the event, so either the fd carries an
   * error or a hang-up, or the direction is ready with no handler of its
   * own. In the first case a handler of the direction takes the event: its
   * own read() or write() reports the condition and, for a socket error,
   * consumes it. With no handler of the direction nothing can take it. */
  if (is_write_direction ? reg->handlers.on_writable != NULL
                         : reg->handlers.on_readable != NULL)
    return _FD_TAKE_DIRECTION;
  _ccol_fd_reg_mute_locked(loop, entry, reg);
  return _FD_TAKE_NONE;
}

/* Mutes reg (see its field comment) and re-arms entry from live state, which
 * drops reg from the interest mask while a sibling direction with handlers
 * stays armed. The caller holds the stripe lock of entry.
 *
 * The re-arm runs at once on both dispatch paths. On the dispatch-pool path
 * it can re-arm an EPOLLONESHOT entry for which the caller is about to
 * submit a job for the sibling direction; an event that the poller collects
 * for the entry before that job finishes is dropped by the in-flight check of
 * _ccol_event_loop_poller_collect, and the post-dispatch re-arm of the job
 * then applies the live state again. The function stays out of line and
 * cold, so the dispatch paths that inline the collection keep their shape. */
static __attribute__((noinline, cold)) void _ccol_fd_reg_mute_locked(
    struct ccol_event_loop_s *loop, event_entry *entry, ccol_event_reg_s *reg) {
  reg->muted = true;
  _ccol_event_loop_rearm_entry_locked(loop, entry);
}

/* Fill item for one direction of an fd entry.
 *
 * EPOLLIN and EPOLLRDHUP are not mutually exclusive with EPOLLERR and
 * EPOLLHUP, and neither is EPOLLOUT. The kernel legitimately reports both on
 * one event. That happens when a peer writes data and then closes or resets
 * the connection at once: the bytes genuinely sit in the receive buffer and
 * are readable, although the peer is already gone. A socket that just entered
 * an error state also reads as writable, because a write(2) on it returns at
 * once instead of blocking.
 *
 * An unconditional priority for the error would therefore discard readable
 * data that a reader was registered to take, and would permanently starve a
 * write-only registration of on_writable. The direction wins only where a
 * handler for that direction exists to consume it. An error with no ready
 * direction goes to on_error when the registration has one, and otherwise to
 * the handler of the direction; see _ccol_fd_reg_takes_event. Everything
 * else that it admits goes out as an error, because no handler on this
 * direction can consume it.
 *
 * That leaves an error-only registration correct in both halves. Such a
 * registration has on_error set and no handler for its own direction, which
 * is how a caller says "tell me when this fd dies, and do not read it". It
 * sees a plain close() by the peer as EPOLLIN | EPOLLRDHUP with no EPOLLHUP,
 * because EPOLLHUP needs both directions down. Routing that to on_error is
 * what stops level-triggered epoll from reporting the identical event for
 * ever with nothing draining the socket. _fd_reg_interest_bits is the other
 * half of the same rule: it never arms EPOLLIN for a registration with no
 * reader, and never arms EPOLLOUT for one with no writer, which is what keeps
 * an error-only registration off the always-writable spin. An is_in with no
 * reader therefore always means that the peer hung up, and never that
 * ordinary data arrived. */
static inline void _ccol_fd_item_fill(_dispatch_item *item,
                                      ccol_event_reg_s *reg, _fd_take take,
                                      bool is_write_direction) {
  bool dispatch_on_direction = (take == _FD_TAKE_DIRECTION);
  item->reg = reg;
  item->is_readable = !is_write_direction && dispatch_on_direction;
  item->is_writable = is_write_direction && dispatch_on_direction;
  item->is_error = !dispatch_on_direction;
  item->sel_snapshot = reg->sel;
}

static void _ccol_event_loop_run_callback(ccol_event_loop loop,
                                          _dispatch_item *item) {
  ccol_event_reg_s *reg = item->reg;
  /* The code checks removed again here, at the real moment of the call. One
   * check at collection time is not enough. That earlier check runs under the
   * stripe lock, before the code adds this item to items[] or to
   * job->items[]. ccol_event_loop_remove() does not take
   * entry->dispatch_lock, and it does not wait for an already-collected item
   * to finish its dispatch. See its own header doc comment. It defers the
   * teardown of a callback that is in progress. That protects the reg and
   * entry memory of ccol_event_loop. It promises nothing about whether a
   * callback that the code collected but did not call yet still fires.
   *
   * A concurrent ccol_event_loop_remove can therefore complete strictly
   * between the collection and the real run of this function. Its caller can
   * then go on and free whatever reg->arg points to. For
   * num_reactor_threads == 1 that window is a handful of instructions, with
   * no thread switch possible in between. The collection and this call happen
   * back to back, in the same function and on the same thread. The window is
   * real but very narrow. For num_reactor_threads > 1 the equivalent window
   * is a ctpool queue wait of any length. That is wide enough to make this a
   * real use-after-free that valgrind catches. The teardown of
   * tests/chttpserver/tests_mem_mgmt.c races the first readable dispatch of a
   * freshly accepted connection against the _close_all_idle_connections step
   * of __chttpsvr_destroy.
   *
   * The conn->reg lifetime discipline of chttpserver is: call
   * ccol_event_loop_remove, then free conn at once. See _conn_close in
   * chttpserver.c. That is only safe when ccol_event_loop guarantees that no
   * callback still runs afterwards with conn as reg->arg. A second check of
   * removed here closes that gap for both dispatch paths with one check. Once
   * removed reads true, this reg never touches reg->arg again. */
  if (atomic_load(&reg->removed)) return;
  /* The code checks paused again here, for the identical reason that it
   * checks removed again above. The contract of ccol_event_loop_pause
   * says that no on_readable, on_writable or on_error callback fires for reg
   * while it is paused. The code enforces that with a new computation of the
   * epoll interest mask of the fd. That only stops a FUTURE epoll_wait from a
   * report of readiness for this reg. It does nothing about an event that the
   * code already collected into a job before the pause() call took effect.
   *
   * For num_reactor_threads == 1 that window is tiny. The collection and this
   * call happen back to back, with no release of a lock in between. For
   * num_reactor_threads > 1 it is a ctpool queue wait of any length. That is
   * wide enough to see in practice under valgrind. The test
   * ccol_event_loop.pause_write_direction shows it. The write end of a pipe
   * is writable from the instant that it exists. The poller can therefore
   * collect it and submit a dispatch job for it before the very next line of
   * the test calls ccol_event_loop_pause. A skip here, and not only at
   * collection time, closes that window in the same way as the removed check
   * above. */
  if (atomic_load(&reg->paused)) return;
  if (item->is_error) {
    if (reg->handlers.on_error)
      reg->handlers.on_error(loop, reg->self_handle, &item->sel_snapshot,
                             reg->arg);
  } else if (item->is_readable) {
    /* This reports readiness only, for every selectable type. The reactor
     * never does the receive itself. The callback does it. See the
     * documentation of ccol_event_readable_fn. */
    if (reg->handlers.on_readable)
      reg->handlers.on_readable(loop, reg->self_handle, &item->sel_snapshot,
                                reg->arg);
  } else if (item->is_writable) {
    if (reg->handlers.on_writable)
      reg->handlers.on_writable(loop, reg->self_handle, &item->sel_snapshot,
                                reg->arg);
  }
}

/* Decrements the refcount of reg after its callback returned. reg does not
 * always have a callback. The function defers reg for a free when this was
 * the last reference and the code already removed reg. See the comment of
 * _ccol_event_loop_defer_reg_free for the reason why a synchronous free here
 * is wrong. refcount is _Atomic, and _ccol_event_loop_defer_reg_free is
 * lock-free. This function therefore needs no lock at all, not even a stripe
 * lock, because nothing here touches entry state.
 *
 * The function pings reclaim_wake_efd for the identical reason that the
 * call of _ccol_event_loop_remove_finish does. With num_reactor_threads > 1,
 * the call site of this function inside
 * _ccol_event_loop_dispatch_job_fn runs on a ctpool WORKER thread, and not
 * on poller_thread. THIS call, and not the decrement of
 * ccol_event_loop_remove, can be the one that really defers reg here.
 * poller_thread then has no other way to learn that at all. It can otherwise
 * stay blocked in its own epoll_wait(-1) indefinitely, with nothing else to
 * wake it. That is exactly the unbounded-latency gap that reclaim_wake_efd
 * exists to close.
 *
 * A ping from the other two call sites of this function is harmless. Both of
 * them already run on poller_thread itself. They are the dispatch for
 * num_reactor_threads == 1 and the unwind of a failed ctpool_submit in
 * _ccol_event_loop_poller_collect. Each of them reaches its own next reclaim
 * point on its own anyway. A redundant self-ping there costs one cheap
 * eventfd write, which it drains on the very next iteration.
 *
 * The code must read nothing off reg between the defer call and the ping.
 * _ccol_event_loop_remove_finish is different. The caller of this function
 * holds no resolve pin on reg. The dispatch collection reads reg directly
 * off the read_reg or write_reg slot of the entry, and never through
 * _ccol_event_reg_resolve. Nothing here therefore stops the
 * _ccol_event_loop_reclaim_pending_frees of poller_thread from a concurrent
 * free of reg. It can free it the instant that
 * _ccol_event_loop_defer_reg_free makes it eligible. For a reg with no
 * resolve call in flight, pending_resolve_count is already 0. A read of
 * reg->handlers.on_removed after that call races exactly that free. That is a
 * real use-after-free, not a theoretical one, and ThreadSanitizer reports it
 * against the async_idle_pool suite of tests/chttpclient. The ping needs
 * no such read, because it is unconditional. */
static void _ccol_event_loop_release_after_dispatch(
    struct ccol_event_loop_s *loop, ccol_event_reg_s *reg) {
  int prev = atomic_fetch_sub(&reg->refcount, 1);
  if (prev == 1 && atomic_load(&reg->removed)) {
    _ccol_event_loop_defer_reg_free(loop, reg);
    /* This ping is unconditional, for the same reason as the ping of
     * _ccol_event_loop_remove_finish. See its comment. Only poller_thread
     * ever reclaims a deferred reg. On an idle loop nothing else brings it
     * back out of epoll_wait. Without this ping, reg stays pending for ever,
     * and so does its still-open bridge_efd. Read nothing off reg here. This
     * call can already have made it eligible for a concurrent free. */
    _eventfd_notify(loop->reclaim_wake_efd);
  }
}

/* Runs the callback of one queue-backed dispatch item. It marks the calling
 * thread as the one that dispatches the queue of that item. A destroy of that
 * queue from inside the callback is then reported as the caller error that it
 * is. Without the mark, that destroy waits on itself for ever. See
 * _queue_wait_for_dispatch_refs. The function clears the mark on the way out.
 * That is what keeps the later, unrelated work of the thread unmarked. Such
 * work is the next dispatch, or a dispatch worker that sits idle. */
static void _queue_dispatch_run_callback(struct ccol_event_loop_s *loop,
                                         _dispatch_item *item) {
  _queue_dispatch_marker_set(_queue_sel_queue(&item->sel_snapshot));
  _ccol_event_loop_run_callback(loop->self_handle, item);
  _queue_dispatch_marker_set(NULL);
}

/* This is the dispatch path for num_reactor_threads == 1 ONLY. There the
 * collection and the dispatch are one synchronous call on the poller thread.
 * No dispatch pool takes part at all. See the field comments of struct
 * ccol_event_loop_s and the branch of _ccol_event_loop_thread_fn on
 * loop->dispatch_pool. For num_reactor_threads > 1 nothing ever calls this
 * function. See _ccol_event_loop_poller_collect for the collection half and
 * _ccol_event_loop_dispatch_job_fn for the dispatch half, which runs on a
 * ctpool worker.
 *
 * The code calls this function once for each epoll_event that epoll_wait
 * returns. It calls it on poller_thread, the only thread that ever calls
 * epoll_wait in either configuration. The function collects the live regs to
 * dispatch under the stripe lock of entry->stripe_idx. It increments the
 * refcount of each one, so nothing can free it while its callback runs. It
 * then releases that lock and runs the callbacks unlocked. It never holds a
 * stripe lock across a callback of the caller.
 *
 * The function holds entry->dispatch_lock across the WHOLE body, and not only
 * across the collection. For num_reactor_threads == 1 that lock is
 * uncontended by construction, because poller_thread is the only caller of
 * this function and no other thread can race it here. The code holds it
 * anyway, so the logic of this function needs no special case for that
 * configuration.
 *
 * The hazard that the lock guards against is a double dispatch. Two threads
 * each dispatch the same entry while it is still ready. Both pass the
 * collection step below for the SAME reg and call its callback at the same
 * time. That is a serialization bug, and not a memory-safety bug. It cannot
 * happen inside this exact function, because only one thread ever calls it,
 * whatever num_reactor_threads holds. The use of this same lock in
 * _ccol_event_loop_dispatch_job_fn closes the equivalent hazard for
 * num_reactor_threads > 1. That use runs on the ctpool worker that really
 * runs a job. See the comment of that function.
 *
 * dispatch_lock lives on the entry, and both directions share it. It
 * therefore also gives the read_reg and write_reg callbacks on the same fd
 * mutual exclusion against each other, and not only against themselves. That
 * is true in both configurations. See the dispatch_lock field comment of
 * the entry for the reason why that guarantee is deliberately strict. */
static void _ccol_event_loop_handle_event(struct ccol_event_loop_s *loop,
                                          ccol_poll_event *ev) {
  event_entry *entry = (event_entry *)ev->data.ptr;

  if (entry == NULL) {
    /* The code leaves the ev.data.ptr of the shutdown eventfd as NULL. That
     * fd exists only to interrupt epoll_wait, and it carries nothing to
     * dispatch. The code deliberately does NOT drain it with a read() here.
     * With more than one reactor thread, a drain deadlocks. Whichever thread
     * processes this event FIRST resets the counter of the eventfd to 0. Any
     * other thread that is blocked in its own epoll_wait call at that moment,
     * with timeout=-1, then has nothing left to see as ready. It never
     * returns, and the join loop of ccol_event_loop_shutdown hangs on it for
     * ever.
     *
     * The code therefore leaves the counter above zero and never drains it.
     * Every epoll_wait call of every thread then keeps seeing this fd as
     * ready, because the registration is level-triggered. That holds in
     * whichever order the threads run, and whether a thread is already
     * blocked or did not call yet. It holds for as long as the process lives,
     * which is exactly what this case needs. A shutdown is one-way, so
     * nothing ever needs to revoke the readiness of this fd. Every thread
     * still reaches the shutting_down check at the top of its loop directly
     * afterwards. No thread therefore spins on this indefinitely. */
    return;
  }

  ccol_mutex_lock(entry->dispatch_lock);

  _dispatch_item items[2];
  size_t n_items = 0;

  /* The code writes entry->stripe_idx once, before it publishes this entry.
   * Publication means the insert into the chmap of a stripe, or the epoll_ctl
   * call. Nothing writes the field again. It is therefore safe to read here
   * with no lock held yet. */
  ccol_event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];
  ccol_mutex_lock(stripe->lock);
  /* Under the stripe lock an fd entry that is not removed has an open fd:
   * the removal takes this lock, and the fd is closed only after it. That
   * makes it safe to ask the fd what it is; see ccol_poll_refine. */
  if (entry->is_fd && !atomic_load(&entry->removed))
    ccol_poll_refine(ev, entry->fd);

  if (entry->is_fd) {
    bool is_err = (ev->events & (CCOL_POLL_ERR | CCOL_POLL_HUP)) != 0;
    bool is_in = (ev->events & (CCOL_POLL_IN | CCOL_POLL_RDHUP)) != 0;
    bool is_out = (ev->events & CCOL_POLL_OUT) != 0;

    /* The rule that turns these three bits into a dispatch is the shared
     * one; see _ccol_fd_item_fill. This path and
     * _ccol_event_loop_poller_collect differ in their locking and in where
     * the work goes, and never in that decision. */
    ccol_event_reg_s *r = entry->as.fd.read_reg;
    _fd_take r_take = _ccol_fd_reg_takes_event(loop, entry, r, is_err, is_in,
                                               /*is_write_direction=*/false);
    if (r_take != _FD_TAKE_NONE) {
      atomic_fetch_add(&r->refcount, 1);
      _ccol_fd_item_fill(&items[n_items], r, r_take,
                         /*is_write_direction=*/false);
      n_items++;
    }
    ccol_event_reg_s *w = entry->as.fd.write_reg;
    _fd_take w_take = _ccol_fd_reg_takes_event(loop, entry, w, is_err, is_out,
                                               /*is_write_direction=*/true);
    if (w_take != _FD_TAKE_NONE) {
      atomic_fetch_add(&w->refcount, 1);
      _ccol_fd_item_fill(&items[n_items], w, w_take,
                         /*is_write_direction=*/true);
      n_items++;
    }
  } else {
    /* A queue entry. The code drains the bridge eventfd here, under the lock
     * of this stripe. A concurrent ccol_event_loop_remove also takes the
     * stripe lock of this exact entry before it touches reg->bridge_efd. It
     * can therefore never read() or close() that fd at the same time.
     * entry->as.reg can legitimately be NULL here. ccol_event_loop_remove
     * sets it to NULL, under this same stripe lock, before it defers the
     * free of the entry. A stale batch entry that reaches this point after a
     * concurrent removal must therefore count as nothing to do. The code must
     * not dereference it. */
    ccol_event_reg_s *r = entry->as.reg;
    if (r && atomic_load(&r->removed)) {
      _queue_reg_consume_wake(r, NULL, NULL);
    } else if (r) {
      _queue_reg_consume_wake(r, &items[n_items].queue_count_before,
                              &items[n_items].wake_hops);
      atomic_fetch_add(&r->refcount, 1);
      _queue_dispatch_ref_acquire(&r->sel);
      items[n_items].reg = r;
      items[n_items].is_error = false;
      items[n_items].is_readable = (r->sel.dir == ccol_select_read);
      items[n_items].is_writable = (r->sel.dir == ccol_select_write);
      items[n_items].sel_snapshot = r->sel;
      n_items++;
    }
  }

  ccol_mutex_unlock(stripe->lock);

  for (size_t i = 0; i < n_items; i++) {
    if (entry->is_fd) {
      _ccol_event_loop_run_callback(loop->self_handle, &items[i]);
    } else {
      _queue_dispatch_run_callback(loop, &items[i]);
      /* This step is for a queue-backed entry only. See the comment of
       * _ccol_event_loop_queue_cascade_notify_next. An fd entry has no waiter
       * list to forward through. The refcount bump of the collection
       * still pins items[i].reg at this call. The code releases that pin
       * strictly below. */
      _ccol_event_loop_queue_cascade_notify_next(
          loop, items[i].reg, items[i].sel_snapshot.dir,
          items[i].queue_count_before, items[i].wake_hops);
      /* The cascade step is the last touch of the queue on this path. The
       * queue reference therefore comes off here, and nothing below reads the
       * queue again. A destroy that blocks on it can go on at once. */
      _queue_dispatch_ref_release(&items[i].sel_snapshot);
    }
    _ccol_event_loop_release_after_dispatch(loop, items[i].reg);
  }

  ccol_mutex_unlock(entry->dispatch_lock);
}

/* One collected dispatch batch for a single entry. It holds up to 2 items,
 * exactly like the stack-local items[2] of
 * _ccol_event_loop_handle_event. The code submits it as one ctpool job. When
 * there are two items, both therefore still run back to back, on one worker
 * and under one acquire of entry->dispatch_lock. That keeps the existing
 * atomicity and ordering of _ccol_event_loop_handle_event for the
 * num_reactor_threads == 1 path exactly. It also matches that path for the
 * > 1 path, instead of a new interleaving that nobody has reasoned about. The
 * job must outlive the collection call of the poller, so that it reaches
 * whichever ctpool worker finally runs it.
 *
 * Each entry owns exactly one job, as the tail of its own allocation. An
 * entry never has more than one job queued or running, because
 * _ccol_event_loop_poller_collect drops every event for an entry whose
 * refcount is above zero. The poller therefore allocates nothing for a
 * dispatch, and no allocation failure can drop one. The job stays owned by
 * its entry until the release of entry->refcount at the end of
 * _ccol_event_loop_dispatch_job_fn; nothing reads it after that release. */
typedef struct _dispatch_job {
  struct ccol_event_loop_s *loop;
  event_entry *entry;
  _dispatch_item items[2];
  size_t n_items;
} _dispatch_job;

/* The layout of one entry allocation of a loop with a dispatch_pool. */
typedef struct _event_entry_with_job {
  event_entry entry;
  _dispatch_job job;
} _event_entry_with_job;

static size_t _ccol_event_loop_entry_alloc_size(
    const struct ccol_event_loop_s *loop) {
  return loop->dispatch_pool ? sizeof(_event_entry_with_job)
                             : sizeof(event_entry);
}

/* The job storage of entry. Valid only for an entry of a loop with a
 * dispatch_pool: see _ccol_event_loop_entry_alloc_size. */
static inline _dispatch_job *_ccol_event_loop_entry_job(event_entry *entry) {
  return &((_event_entry_with_job *)entry)->job;
}

/* A forward declaration. _ccol_event_loop_poller_collect below submits jobs
 * to this function with ctpool_submit. It does that before the definition
 * of this function appears later in this file. */
static void _ccol_event_loop_dispatch_job_fn(void *arg);

/* A thread-local key for the whole process. The code creates it on first use.
 * On a dispatch_pool worker it holds the ccol_event_loop, as an opaque
 * `void *`, whose dispatch job runs on the calling thread now, and NULL when
 * no such job runs. On the poller thread of a loop it holds that loop for the
 * whole life of the thread, and _ccol_event_loop_thread_fn clears it before
 * the thread ends. Every other thread holds NULL. Only the self-call guards
 * of ccol_event_loop_shutdown and ccol_event_loop_destroy read it. See the
 * comment of the first one.
 *
 * The key covers the whole process on purpose, and not one loop. One
 * ccol_event_loop instance owns each ctpool worker thread privately, through
 * its own dispatch_pool, for the whole lifetime of that thread. A ctpool
 * always spawns and owns its own dedicated OS threads, and never shares them
 * across pools. One process-wide key therefore answers "which job of which
 * loop runs on me right now" correctly for every ccol_event_loop instance in
 * the process. That includes the case of several independent instances. There
 * is no per-loop key lifecycle to manage.
 *
 * The destruction of a single loop never tears the key down. live and the
 * deletion follow the queue dispatch marker key exactly: see the comment of
 * queue_dispatch_marker_bundle. ccol_event_loop_create_with_mprocs refuses to
 * build a loop while live is false, and every other use of the key comes from
 * a live loop. */
static struct {
  ccol_thread_ls_key_t key;
  ccol_once_flag_t once;
  atomic_bool live;
} ccol_event_loop_job_key_bundle = {.once = CCOL_ONCE_INIT};

static void _ccol_event_loop_init_job_key(void) {
  if (ccol_thread_ls_key_create(ccol_event_loop_job_key_bundle.key, NULL) == 0)
    atomic_store(&ccol_event_loop_job_key_bundle.live, true);
}

/* The poller path for num_reactor_threads > 1. It collects the dispatch items
 * for entry under the stripe lock of that entry only. It takes no
 * entry->dispatch_lock. See the design comment above
 * _ccol_event_loop_handle_event for the reason why the
 * num_reactor_threads == 1 path needs that lock there. This path structurally
 * cannot race itself in the same way. There is exactly one poller_thread, so
 * a collection is always serial. A ctpool worker takes
 * entry->dispatch_lock only later, when it really runs the job. That mirrors
 * ccol_event_loop_modify, which also never takes it. See the comment of
 * that function.
 *
 * The function then bumps the refcount of entry. That protects the job that
 * it is about to submit from a concurrent ccol_event_loop_remove or reclaim.
 * See the comment of event_entry.refcount. Finally it submits the job that
 * entry owns to dispatch_pool, and a worker runs it.
 *
 * It returns true when the submit failed. The event is then dropped and the
 * entry re-armed, and the caller backs off before its next epoll_wait. See
 * _ccol_event_loop_dispatch_backoff.
 *
 * This function is deliberately almost identical to the collection logic
 * of _ccol_event_loop_handle_event, and it shares no code with it. The
 * num_reactor_threads == 1 path must stay free of every dispatch-pool
 * concern. That is a hard requirement. See the field comment of struct
 * ccol_event_loop_s. This file therefore carries two distinct dispatch code
 * paths, and not one unified implementation. That is a deliberate trade of
 * complexity for correctness and performance. It is not a free
 * simplification, and it is a real, ongoing maintenance surface.
 *
 * _ccol_event_loop_handle_event is different in one more way. This function
 * does NOT drain the bridge eventfd of a queue selectable. See the
 * comment of _ccol_event_loop_dispatch_job_fn for the reason why that drain
 * must wait until the job really runs on a worker. */
static bool _ccol_event_loop_poller_collect(struct ccol_event_loop_s *loop,
                                            ccol_poll_event *ev) {
  event_entry *entry = (event_entry *)ev->data.ptr;
  if (entry == NULL)
    return false; /* The shutdown sentinel. See the identical comment in
                     _ccol_event_loop_handle_event. */

  ccol_event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];
  ccol_mutex_lock(stripe->lock);
  /* Under the stripe lock an fd entry that is not removed has an open fd:
   * the removal takes this lock, and the fd is closed only after it. That
   * makes it safe to ask the fd what it is; see ccol_poll_refine. */
  if (entry->is_fd && !atomic_load(&entry->removed))
    ccol_poll_refine(ev, entry->fd);

  /* A job for this entry is already in the queue or already runs. The code
   * therefore drops this event completely. It does not submit a second,
   * concurrent job. Without this check the result is a real use-after-free,
   * and valgrind catches it.
   *
   * Application code may call ccol_event_loop_modify from WITHIN a callback
   * that is in flight, and it does. The TLS and request state machine of
   * chttpclient.c flips WANT_READ and WANT_WRITE as it advances.
   * ccol_event_loop_modify re-arms EPOLLONESHOT through this same shared
   * helper. See _ccol_event_loop_rearm_entry_locked. That is the same helper
   * as for a genuine post-dispatch re-arm. The fd can already be ready again
   * at that moment, because a real response can arrive within microseconds on
   * loopback. The poller can then collect and submit a SECOND job for this
   * entry while the callback of the FIRST job still runs. That first callback
   * is about to free application state that the callback of the second job
   * then reads. The second callback is correctly serialised to run strictly
   * AFTER the first. But the first releases dispatch_lock only once that
   * state is already freed.
   *
   * entry->refcount answers exactly one question: is there already a job for
   * this entry that did not reach that point yet? The code bumps it below. It
   * releases it only once both steps of the in-flight job finish: the
   * dispatch_lock-protected callback and the post-dispatch re-arm. See
   * _ccol_event_loop_dispatch_job_fn. The check reads it under the same
   * stripe lock that guards the matching decrement, so there is no window in
   * which it looks inconsistent.
   *
   * A drop of this event is safe. The eventual re-arm of the in-flight
   * job, and not this one, reflects the state correctly. It reflects the
   * state at the moment where that job really finishes. A genuinely new
   * readiness after that point comes back fresh. */
  if (atomic_load(&entry->refcount) > 0) {
    ccol_mutex_unlock(stripe->lock);
    return false;
  }

  /* refcount is 0 under the stripe lock, so no job of this entry is queued
   * or running, and its job storage is free to fill. */
  _dispatch_job *job = _ccol_event_loop_entry_job(entry);
  job->loop = loop;
  job->entry = entry;
  job->n_items = 0;

  if (entry->is_fd) {
    bool is_err = (ev->events & (CCOL_POLL_ERR | CCOL_POLL_HUP)) != 0;
    bool is_in = (ev->events & (CCOL_POLL_IN | CCOL_POLL_RDHUP)) != 0;
    bool is_out = (ev->events & CCOL_POLL_OUT) != 0;

    /* This mirrors the dispatch-priority logic of
     * _ccol_event_loop_handle_event for EPOLLIN against EPOLLERR and
     * EPOLLHUP, exactly. See the comment of that function for the full
     * reasoning. Readable wins only when a real reader is registered to take
     * the data. Every other condition that the collection guard admits goes
     * out as an error. The code does not drop it and leave level-triggered
     * epoll to report it again for ever. */
    /* The same shared rule as _ccol_event_loop_handle_event; see
     * _ccol_fd_item_fill. */
    ccol_event_reg_s *r = entry->as.fd.read_reg;
    _fd_take r_take = _ccol_fd_reg_takes_event(loop, entry, r, is_err, is_in,
                                               /*is_write_direction=*/false);
    if (r_take != _FD_TAKE_NONE) {
      atomic_fetch_add(&r->refcount, 1);
      _ccol_fd_item_fill(&job->items[job->n_items], r, r_take,
                         /*is_write_direction=*/false);
      job->n_items++;
    }
    ccol_event_reg_s *w = entry->as.fd.write_reg;
    _fd_take w_take = _ccol_fd_reg_takes_event(loop, entry, w, is_err, is_out,
                                               /*is_write_direction=*/true);
    if (w_take != _FD_TAKE_NONE) {
      atomic_fetch_add(&w->refcount, 1);
      _ccol_fd_item_fill(&job->items[job->n_items], w, w_take,
                         /*is_write_direction=*/true);
      job->n_items++;
    }
  } else {
    /* A queue entry. entry->as.reg can legitimately be NULL here. A
     * concurrent ccol_event_loop_remove sets it to NULL under this exact
     * stripe lock, before it defers the free of the entry. See the
     * identical check in _ccol_event_loop_handle_event for the reason. The
     * code deliberately defers the drain of the bridge eventfd to
     * _ccol_event_loop_dispatch_job_fn. It also claims no reference to the
     * queue here; see _queue_dispatch_ref_acquire for why only the job
     * itself, once it runs, may claim one. */
    ccol_event_reg_s *r = entry->as.reg;
    if (r && !atomic_load(&r->removed)) {
      atomic_fetch_add(&r->refcount, 1);
      _dispatch_item *item = &job->items[job->n_items];
      item->reg = r;
      item->is_error = false;
      item->is_readable = (r->sel.dir == ccol_select_read);
      item->is_writable = (r->sel.dir == ccol_select_write);
      item->sel_snapshot = r->sel;
      job->n_items++;
    }
  }

  if (job->n_items == 0) {
    /* There is nothing live to dispatch. For example, the code already
     * removed every candidate reg by the time that the collection ran. It
     * bumped no entry refcount, and there is nothing to submit. Note: the
     * code leaves this entry un-rearmed when EPOLLONESHOT already fired for
     * it. That is correct. With no live reg left, nothing must ever be
     * notified for it again. The EPOLL_CTL_DEL and EPOLL_CTL_MOD calls of
     * ccol_event_loop_remove own the real interest state of this entry from
     * here on. */
    ccol_mutex_unlock(stripe->lock);
    return false;
  }

  atomic_fetch_add(&entry->refcount, 1);
  ccol_mutex_unlock(stripe->lock);

  ccol_retval_t rv = ctpool_submit(loop->dispatch_pool,
                                   _ccol_event_loop_dispatch_job_fn, job, NULL);
  if (rv != ccol_success) {
    /* ccol_not_enough_memory is the only real failure mode of ctpool_submit
     * on the always-unbounded queue of this module: the pool could not
     * allocate a task node. The code unwinds exactly what it bumped above and
     * drops the dispatch. It then takes the stripe lock again, which it
     * released above before the submit call, and re-arms. The kernel disarms
     * an EPOLLONESHOT registration the moment that it reports an event,
     * whatever userspace does with that event. Without the re-arm, the entry
     * is never reported again, and not merely delayed. The code releases the
     * refcount and re-arms together under the lock. That matches the pairing
     * of _ccol_event_loop_dispatch_job_fn, for the identical reason. See the
     * comment of that function.
     *
     * A level-triggered fd that is still ready reports again on the very
     * next epoll_wait. The true return makes the poller back off before that
     * wait, so a persistent allocation failure cannot spin it. */
    for (size_t i = 0; i < job->n_items; i++)
      _ccol_event_loop_release_after_dispatch(loop, job->items[i].reg);
    ccol_mutex_lock(stripe->lock);
    _ccol_event_loop_rearm_entry_locked(loop, entry);
    atomic_fetch_sub(&entry->refcount, 1);
    ccol_mutex_unlock(stripe->lock);
    return true;
  }
  return false;
}

/* The ctpool task function, for num_reactor_threads > 1 only. It runs on a
 * dispatch_pool worker thread, one job at a time. It locks
 * entry->dispatch_lock around the real callback calls. The poller never holds
 * that lock. See the comment of _ccol_event_loop_poller_collect. For a queue
 * item it also claims the reference to the queue that a destroy of that
 * queue waits for, and only here; see _queue_dispatch_ref_acquire.
 *
 * The function drains the bridge eventfd of a queue item here, and not at
 * collection time. That is deliberate. A failed submit in
 * _ccol_event_loop_poller_collect then never silently loses a queue
 * notification. The refcount of reg, which the code already bumped at
 * collection time, keeps bridge_efd alive and valid for this read. That holds
 * whatever a concurrent ccol_event_loop_remove does. The function then
 * re-arms the EPOLLONESHOT interest through the shared helper, before it
 * releases the refcount of entry.
 *
 * That order matters. The re-arm happens strictly BEFORE the release of the
 * refcount of entry. There are two reasons. The re-arm step itself needs
 * entry to still be a valid, live struct. And
 * _ccol_event_loop_reclaim_pending_frees, the only place that ever really
 * frees an entry, checks refcount == 0. A release first lets a concurrent
 * reclaim free entry out from under a re-arm that is still in flight on this
 * thread. */
static void _ccol_event_loop_dispatch_job_fn(void *arg) {
  _dispatch_job *job = (_dispatch_job *)arg;
  struct ccol_event_loop_s *loop = job->loop;
  event_entry *entry = job->entry;

  ccol_call_once(ccol_event_loop_job_key_bundle.once,
                 _ccol_event_loop_init_job_key);
  ccol_thread_ls_set(ccol_event_loop_job_key_bundle.key, (void *)loop);

  ccol_event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];

  ccol_mutex_lock(entry->dispatch_lock);
  for (size_t i = 0; i < job->n_items; i++) {
    _dispatch_item *item = &job->items[i];
    if (entry->is_fd) {
      _ccol_event_loop_run_callback(loop->self_handle, item);
    } else {
      /* The reference to the queue is claimed here, when the job runs, under
       * the stripe lock that also orders it against the removed store of
       * ccol_event_loop_remove. See _queue_dispatch_ref_acquire. A removed
       * registration claims nothing and touches the queue not at all: the
       * application may already have destroyed that queue, which is exactly
       * what a destroy that waited for this queued job could never let
       * happen. The removal also took the bridge eventfd out of epoll, so
       * nothing is left to drain. */
      ccol_mutex_lock(stripe->lock);
      bool live = !atomic_load(&item->reg->removed);
      if (live) {
        _queue_dispatch_ref_acquire(&item->sel_snapshot);
        _queue_reg_consume_wake(item->reg, &item->queue_count_before,
                                &item->wake_hops);
      }
      ccol_mutex_unlock(stripe->lock);
      if (live) {
        _queue_dispatch_run_callback(loop, item);
        /* See the identical calls in _ccol_event_loop_handle_event. They
         * explain why both of these are for a queue-backed entry only. They
         * also explain why the cascade must run before the pin release
         * below. The cascade is the last touch of the queue on this path,
         * and the queue reference comes off only after it returns. */
        _ccol_event_loop_queue_cascade_notify_next(
            loop, item->reg, item->sel_snapshot.dir, item->queue_count_before,
            item->wake_hops);
        _queue_dispatch_ref_release(&item->sel_snapshot);
      }
    }
    _ccol_event_loop_release_after_dispatch(loop, item->reg);
  }
  ccol_mutex_unlock(entry->dispatch_lock);

  /* The re-arm and the release of the refcount happen together, under the
   * same stripe lock. The "a job is already in flight for this entry"
   * check of _ccol_event_loop_poller_collect reads entry->refcount under this
   * exact lock. It must never see "already re-armed" while the refcount is
   * still above zero. In that window the re-arm is done and the decrement is
   * not yet visible to a poller that collects at the same time. That poller
   * then wrongly treats a genuinely new readiness event, which arrived after
   * the re-arm, as "still in flight". It silently drops it. EPOLLONESHOT
   * already consumed that notification, and nothing is left to re-arm it.
   * That is a real failure, and a quieter one than the use-after-free that
   * this refcount check exists to prevent. */
  ccol_mutex_lock(stripe->lock);
  _ccol_event_loop_rearm_entry_locked(loop, entry);
  atomic_fetch_sub(&entry->refcount, 1);
  ccol_mutex_unlock(stripe->lock);
  /* entry owns job. From the release above on, the poller can fill job for a
   * new dispatch, and a reclaim can free entry with it, so nothing below
   * touches either. */

  ccol_thread_ls_set(ccol_event_loop_job_key_bundle.key, NULL);
}

/* The retry interval for the next epoll_wait when
 * _ccol_event_loop_reclaim_pending_frees leaves anything still pending. This
 * is a correctness backstop. See the comment of that function for the
 * reason why a ping alone is not reliable. It is not a latency target that
 * this module is tuned around. The value 50ms is chosen for two reasons. It
 * is comfortably smaller than the timeout budget of every existing
 * on_removed latency test. It is also large enough that a registration which
 * is genuinely stuck for a long time costs no meaningful busy-poll. Such a
 * registration comes from an application bug elsewhere, and not from this
 * module. The cost is one return from epoll_wait plus one lock-free list scan
 * every 50ms. The code pays it only while at least one entry or reg is still
 * pending. */
#define EVENT_LOOP_RECLAIM_RETRY_MS 50

/* The first and the largest pause of _ccol_event_loop_dispatch_backoff. */
#define EVENT_LOOP_DISPATCH_BACKOFF_MIN_MS 1
#define EVENT_LOOP_DISPATCH_BACKOFF_MAX_MS 64

/* Pauses poller_thread after a batch in which a dispatch could not be
 * submitted. The entry of that dispatch is re-armed, and a level-triggered fd
 * that is still ready reports again on the very next epoll_wait. Without a
 * pause, a persistent allocation failure turns the poller into a busy loop
 * that burns a whole CPU and makes no progress. The pause doubles on each
 * consecutive failed batch, from EVENT_LOOP_DISPATCH_BACKOFF_MIN_MS up to
 * EVENT_LOOP_DISPATCH_BACKOFF_MAX_MS, and a batch with no failure resets it.
 * It waits on shutdown_efd, which a shutdown writes and nothing drains, so a
 * shutdown ends the pause at once. */
static __attribute__((noinline, cold)) void _ccol_event_loop_dispatch_backoff(
    struct ccol_event_loop_s *loop, int *backoff_ms) {
  *backoff_ms =
      *backoff_ms == 0 ? EVENT_LOOP_DISPATCH_BACKOFF_MIN_MS : *backoff_ms * 2;
  if (*backoff_ms > EVENT_LOOP_DISPATCH_BACKOFF_MAX_MS)
    *backoff_ms = EVENT_LOOP_DISPATCH_BACKOFF_MAX_MS;
  struct pollfd pfd = {.fd = loop->shutdown_efd, .events = POLLIN};
  (void)poll(&pfd, 1, *backoff_ms);
}

static void *_ccol_event_loop_thread_fn(void *arg) {
  struct ccol_event_loop_s *loop = (struct ccol_event_loop_s *)arg;
  ccol_poll_event *events = loop->poller_events;
  int backoff_ms = 0;

  /* The mark that identifies this thread as the poller of loop, for the
   * self-call guards of ccol_event_loop_shutdown and ccol_event_loop_destroy.
   * It holds for the whole life of this thread: callbacks and on_removed
   * notifications run here. The key is live, because
   * ccol_event_loop_create_with_mprocs refuses to build a loop otherwise. */
  ccol_thread_ls_set(ccol_event_loop_job_key_bundle.key, (void *)loop);

  for (;;) {
    if (atomic_load(&loop->shutting_down)) break;

    /* This is the point between two batches. poller_thread fully walked its
     * previous batch by now, when it had one. The code publishes that fact:
     * it advances poller_batch_gen. It then tries to reclaim the deferred
     * entries and regs that this advance now proves safe to free. See the
     * large comment above _ccol_event_loop_reclaim_pending_frees for the full
     * design. */
    atomic_fetch_add(&loop->poller_batch_gen, (uint64_t)1);
    bool reclaim_still_pending = _ccol_event_loop_reclaim_pending_frees(loop);

    /* The code uses EVENT_LOOP_RECLAIM_RETRY_MS instead of -1 whenever
     * anything is still pending. See the comment of
     * _ccol_event_loop_reclaim_pending_frees for the reason. This bounded
     * retry is what guarantees forward progress here. A perfectly timed ping
     * does not. */
    int wait_timeout_ms =
        reclaim_still_pending ? EVENT_LOOP_RECLAIM_RETRY_MS : -1;
    int n = ccol_poll_wait(loop->epfd, events, (int)loop->max_events_per_wait,
                           wait_timeout_ms);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&loop->poller_iterations_for_tests, (uint64_t)1);
#endif
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    bool dispatch_failed = false;
    for (int i = 0; i < n; i++) {
      /* The sentinel of reclaim_wake_efd. See its field comment. There is
       * nothing to dispatch. The code only drains it, so that another ping
       * can wake the poller later. The reclaim at the top of this loop
       * already ran above, unconditionally, before this epoll_wait call. It
       * picks up whatever this ping was for on the NEXT iteration. This check
       * comes before the shutdown-sentinel branch inside
       * _ccol_event_loop_handle_event and _ccol_event_loop_poller_collect.
       * That branch tests ev.data.ptr == NULL, and the ev.data.ptr of
       * this fd is never NULL. */
      if (events[i].data.ptr == &loop->reclaim_wake_efd) {
        _eventfd_drain(loop->reclaim_wake_efd);
        continue;
      }
      if (loop->dispatch_pool) {
        dispatch_failed |= _ccol_event_loop_poller_collect(loop, &events[i]);
      } else {
        _ccol_event_loop_handle_event(loop, &events[i]);
      }
    }
    if (__builtin_expect(dispatch_failed, 0))
      _ccol_event_loop_dispatch_backoff(loop, &backoff_ms);
    else
      backoff_ms = 0;
  }

  /* The mark goes before the thread ends, so no later thread can carry it.
   * glibc hands the descriptor, and so the thread ID, of a joined thread to
   * the next thread that starts, which is why the guards never compare
   * ccol_get_thread_id() with poller_thread. */
  ccol_thread_ls_set(ccol_event_loop_job_key_bundle.key, NULL);
  return NULL;
}

/* Destroys the first `created` stripes of loop->stripes. Each one carries a
 * mutex and a chmap. The function then frees the array itself. It uses the
 * raw mmgmt_procs parameter, and not loop->m_procs. That matches every other
 * _ccol_mem_free call site in ccol_event_loop_create_with_mprocs. Some of
 * those run before the code even fills loop->m_procs in. The code uses this
 * function only to roll back a failure at creation time. The stripe
 * teardown of __ccol_event_loop_destroy does more work: it frees the live
 * entries first. It is not built on this function. */
static void _destroy_stripes(struct ccol_event_loop_s *loop,
                             ccol_memmgmt_procs_t *mmgmt_procs,
                             size_t created) {
  for (size_t i = 0; i < created; i++) {
    chmap_destroy(loop->stripes[i].fd_index);
    ccol_mutex_destroy(loop->stripes[i].lock);
  }
  _ccol_mem_free(mmgmt_procs, loop->stripes);
}

/* A forward declaration. The definition sits below ccol_event_loop_shutdown
 * and _ccol_event_loop_shutdown_internal. The declaration is needed here for
 * the rollback of ccol_event_loop_create_with_mprocs after a failed slot
 * acquire. */
static void _ccol_event_loop_teardown_raw(struct ccol_event_loop_s *loop);

ccol_event_loop ccol_event_loop_create_with_mprocs(
    size_t max_events_per_wait, size_t num_lock_stripes,
    size_t num_reactor_threads, ccol_memmgmt_procs_t *mmgmt_procs,
    char **err_str) {
  if (max_events_per_wait == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("max_events_per_wait must be positive");
    return CCOL_EVENT_LOOP_INVALID;
  }
  /* The code narrows max_events_per_wait to a plain `int` for the
   * maxevents parameter of epoll_wait, in _ccol_event_loop_thread_fn. It also
   * uses the value to size the events buffer of the poller thread below.
   * That allocation is max_events_per_wait * sizeof(struct epoll_event).
   *
   * An unreasonably large value causes one of two failures. The (int) cast
   * can produce a maxevents that is zero or negative, and epoll_wait then
   * fails with EINVAL. Or max_events_per_wait * sizeof(struct epoll_event)
   * can overflow size_t on an ILP32 platform. The code then hands a buffer
   * that is too small to a maxevents value that the kernel believes is much
   * larger. The function _ccol_event_loop_thread_fn treats a failed
   * epoll_wait as a permanent exit of the poller thread, on its very first
   * iteration. The caller of this constructor never learns about it. The
   * caller then holds a handle that looks valid and that never dispatches
   * anything.
   *
   * The code therefore rejects such a value here. This mirrors the
   * SIZE_MAX / sizeof(x) overflow-guard convention that
   * verify_circular_queue_create_inputs already uses for the identical class
   * of hazard. */
  if (max_events_per_wait > (size_t)INT_MAX) {
    if (err_str)
      *err_str = CCOL_ERR_STR("max_events_per_wait must not exceed INT_MAX");
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (max_events_per_wait > SIZE_MAX / sizeof(ccol_poll_event)) {
    if (err_str)
      *err_str = CCOL_ERR_STR(
          "max_events_per_wait is too large: max_events_per_wait * "
          "sizeof(ccol_poll_event) would overflow size_t");
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (num_lock_stripes == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("num_lock_stripes must be positive");
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (num_reactor_threads == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("num_reactor_threads must be positive");
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return CCOL_EVENT_LOOP_INVALID;
  }

  /* The dispatch of a loop sets two thread-local keys of this module. A key
   * that could not be created names no key of this module, so a loop that
   * would set it is refused. See queue_dispatch_marker_bundle. */
  ccol_call_once(ccol_event_loop_job_key_bundle.once,
                 _ccol_event_loop_init_job_key);
  if (!atomic_load(&ccol_event_loop_job_key_bundle.live) ||
      !_queue_dispatch_marker_available()) {
    if (err_str)
      *err_str = CCOL_ERR_STR(
          "failed to create a thread-local key of ccol_event_loop");
    return CCOL_EVENT_LOOP_INVALID;
  }

  struct ccol_event_loop_s *loop = (struct ccol_event_loop_s *)_ccol_mem_alloc(
      mmgmt_procs, sizeof(struct ccol_event_loop_s));
  if (!loop) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate memory for ccol_event_loop");
    return CCOL_EVENT_LOOP_INVALID;
  }

  if (!ccol_populate_mem_mgmt_procs(loop, mmgmt_procs, err_str)) {
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  loop->epfd = ccol_poll_create();
  if (loop->epfd < 0) {
    if (err_str) *err_str = CCOL_ERR_STR("epoll_create1 failed");
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  loop->shutdown_efd = ccol_wakefd_create();
  if (loop->shutdown_efd < 0) {
    if (err_str) *err_str = CCOL_ERR_STR("eventfd failed");
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  ccol_poll_event ev;
  ev.data.ptr = NULL;
  ev.events = CCOL_POLL_IN;
  if (ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_ADD, loop->shutdown_efd, &ev) <
      0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("epoll_ctl failed registering shutdown eventfd");
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  loop->reclaim_wake_efd = ccol_wakefd_create();
  if (loop->reclaim_wake_efd < 0) {
    if (err_str) *err_str = CCOL_ERR_STR("eventfd failed");
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  ev.data.ptr = &loop->reclaim_wake_efd;
  ev.events = CCOL_POLL_IN;
  if (ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_ADD, loop->reclaim_wake_efd,
                    &ev) < 0) {
    if (err_str)
      *err_str =
          CCOL_ERR_STR("epoll_ctl failed registering reclaim-wake eventfd");
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  if (ccol_mutex_init(loop->shutdown_lock) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to initialize shutdown_lock");
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (ccol_cond_var_init(loop->joined_cv) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to initialize joined_cv");
    ccol_mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (ccol_rw_lock_init(loop->reg_slot_rwlock) != 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to initialize reg_slot_rwlock");
    ccol_cond_var_destroy(loop->joined_cv);
    ccol_mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }
  loop->reg_slots =
      cvector_create_full(sizeof(ccol_event_reg_slot_t), mmgmt_procs, NULL);
  if (!loop->reg_slots) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to allocate reg slot array");
    ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    ccol_cond_var_destroy(loop->joined_cv);
    ccol_mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }
  loop->reg_lost_indices = 0;
  loop->reg_free_indices =
      cvector_create_full(sizeof(uint32_t), mmgmt_procs, NULL);
  if (!loop->reg_free_indices) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate reg free-index array");
    __cvector_destroy(loop->reg_slots);
    ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    ccol_cond_var_destroy(loop->joined_cv);
    ccol_mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }
  loop->shutdown_started = false;
  loop->joined = false;
  loop->destroy_claimed = false;
  atomic_init(&loop->shutting_down, false);
  loop->max_events_per_wait = max_events_per_wait;
  atomic_init(&loop->pending_entry_frees, NULL);
  atomic_init(&loop->pending_reg_frees, NULL);
  atomic_init(&loop->poller_batch_gen, (uint64_t)0);
  atomic_init(&loop->fd_generation_counter, (uint64_t)0);
  atomic_init(&loop->next_queue_stripe, (size_t)0);
  atomic_init(&loop->reg_count, (size_t)0);
#ifdef RUNNING_UNIT_TESTS
  /* The code must initialise this strictly before it creates poller_thread
   * below. pending_resolve_count and foreign_since_fork are different. The
   * code initialises both further down, after ccol_thread_create, and that is
   * safe because poller_thread never touches either. poller_thread DOES write
   * this field on every single loop iteration. An atomic_init after that
   * thread can already run is therefore itself a data race. */
  atomic_init(&loop->poller_iterations_for_tests, (uint64_t)0);
  atomic_init(&loop->entries_reclaimed_for_tests, (uint64_t)0);
#endif
  loop->num_stripes = num_lock_stripes;
  loop->num_reactor_threads = num_reactor_threads;
  loop->dispatch_pool = CTPOOL_INVALID;

  loop->stripes = _ccol_mem_calloc(mmgmt_procs, num_lock_stripes,
                                   sizeof(ccol_event_loop_stripe_t));
  if (!loop->stripes) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate lock stripe array");
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    ccol_cond_var_destroy(loop->joined_cv);
    ccol_mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  size_t stripes_created = 0;
  for (; stripes_created < num_lock_stripes; stripes_created++) {
    char *stripe_err = NULL;
    loop->stripes[stripes_created].fd_index =
        chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_int,
                          ccol_pointer, mmgmt_procs, NULL, NULL, &stripe_err);
    if (!loop->stripes[stripes_created].fd_index) {
      if (err_str)
        *err_str = stripe_err ? stripe_err
                              : CCOL_ERR_STR("Failed to create fd registry");
      _destroy_stripes(loop, mmgmt_procs, stripes_created);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      ccol_rw_lock_destroy(loop->reg_slot_rwlock);
      ccol_cond_var_destroy(loop->joined_cv);
      ccol_mutex_destroy(loop->shutdown_lock);
      close(loop->reclaim_wake_efd);
      close(loop->shutdown_efd);
      close(loop->epfd);
      _ccol_mem_free(mmgmt_procs, loop->m_procs);
      _ccol_mem_free(mmgmt_procs, loop);
      return CCOL_EVENT_LOOP_INVALID;
    }
    if (ccol_mutex_init(loop->stripes[stripes_created].lock) != 0) {
      if (err_str) *err_str = CCOL_ERR_STR("Failed to initialize stripe lock");
      /* The _destroy_stripes(loop, mmgmt_procs, stripes_created) call below
       * only destroys the stripes [0, stripes_created). The current stripe
       * already has an fd_index, and nothing validly initialised its .lock.
       * The code therefore tears that fd_index down explicitly first. The
       * sibling !fd_index failure block above handles its own current stripe
       * in exactly the same way. */
      chmap_destroy(loop->stripes[stripes_created].fd_index);
      _destroy_stripes(loop, mmgmt_procs, stripes_created);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      ccol_rw_lock_destroy(loop->reg_slot_rwlock);
      ccol_cond_var_destroy(loop->joined_cv);
      ccol_mutex_destroy(loop->shutdown_lock);
      close(loop->reclaim_wake_efd);
      close(loop->shutdown_efd);
      close(loop->epfd);
      _ccol_mem_free(mmgmt_procs, loop->m_procs);
      _ccol_mem_free(mmgmt_procs, loop);
      return CCOL_EVENT_LOOP_INVALID;
    }
    loop->stripes[stripes_created].queue_regs_head = NULL;
  }

  /* The epoll_wait buffer of poller_thread. It is allocated here, before the
   * thread starts, so that the thread performs no step that can fail. A
   * handle that this function returns therefore always names a loop that
   * dispatches. The size cannot overflow: see the checks on
   * max_events_per_wait at the top of this function. */
  loop->poller_events = _ccol_mem_alloc(
      mmgmt_procs, max_events_per_wait * sizeof(ccol_poll_event));
  if (!loop->poller_events) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate the epoll_wait buffer");
    _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    ccol_cond_var_destroy(loop->joined_cv);
    ccol_mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  /* dispatch_pool exists only for num_reactor_threads > 1. See the field
   * comment of struct ccol_event_loop_s. Its size is
   * num_reactor_threads - 1. The total OS thread count therefore stays
   * exactly num_reactor_threads: 1 for poller_thread plus this pool. The
   * parameter keeps its documented meaning for resource usage. The queue of
   * the pool has no bound, because queue_capacity is 0. The poller must never
   * block on a submission. See the comment of
   * _ccol_event_loop_poller_collect on the failure handling of
   * ctpool_submit. The code creates the pool before it spawns poller_thread
   * below. dispatch_pool is therefore always fully valid before any event can
   * reach it. */
  if (num_reactor_threads > 1) {
    char *pool_err = NULL;
    loop->dispatch_pool = ccol_create_cthread_pool_mp(
        num_reactor_threads - 1, 0, mmgmt_procs, &pool_err);
    if (!loop->dispatch_pool) {
      if (err_str)
        *err_str = pool_err ? pool_err
                            : CCOL_ERR_STR("Failed to create dispatch pool");
      _ccol_mem_free(mmgmt_procs, loop->poller_events);
      _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      ccol_rw_lock_destroy(loop->reg_slot_rwlock);
      ccol_cond_var_destroy(loop->joined_cv);
      ccol_mutex_destroy(loop->shutdown_lock);
      close(loop->reclaim_wake_efd);
      close(loop->shutdown_efd);
      close(loop->epfd);
      _ccol_mem_free(mmgmt_procs, loop->m_procs);
      _ccol_mem_free(mmgmt_procs, loop);
      return CCOL_EVENT_LOOP_INVALID;
    }
  }

  if (ccol_thread_create(loop->poller_thread, _ccol_event_loop_thread_fn,
                         loop) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("pthread_create failed");
    if (loop->dispatch_pool) __ctpool_destroy(loop->dispatch_pool);
    _ccol_mem_free(mmgmt_procs, loop->poller_events);
    _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    ccol_cond_var_destroy(loop->joined_cv);
    ccol_mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  atomic_init(&loop->pending_resolve_count, (size_t)0);
#if CCOL_FORK_SAFETY_REQUIRED
  atomic_init(&loop->foreign_since_fork, false);
#endif

  /* The slot acquire is the LITERAL LAST step. It runs after the poller
   * thread started successfully, and after dispatch_pool started too when the
   * caller configured one. This mirrors the constructors of chttpcli and
   * chttpsvr exactly. No handle therefore reaches any caller until this
   * function is already about to return success.
   *
   * A failure here must NOT take the same path as the ordinary allocation
   * failures above. The poller thread already runs, and it can already have a
   * dispatch_pool with its own workers. The rollback must really stop them.
   * The code therefore reuses _ccol_event_loop_teardown_raw, the same helper
   * that __ccol_event_loop_destroy uses. Its own call to
   * _ccol_event_loop_shutdown_internal does exactly that. A plain free of the
   * memory under a thread that is still live is wrong.
   *
   * The call needs no wait on pending_resolve_count of any kind. No handle
   * reached any caller at this point. Nothing can therefore have resolved it,
   * and nothing can have pinned it. */
  ccol_event_loop h = _ccol_event_loop_handle_slot_acquire(loop);
  if (h == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate ccol_event_loop handle slot");
    _ccol_event_loop_teardown_raw(loop);
    return CCOL_EVENT_LOOP_INVALID;
  }
  loop->self_handle = h;

  if (err_str) *err_str = NULL;
  return h;
}

/* The real shutdown body. It takes an already-resolved raw pointer directly.
 * Two callers use it. The first is the thin public wrapper below, after its
 * resolve and pin. The second is _ccol_event_loop_teardown_raw. Two places
 * call that helper, and neither of them resolves a handle again. They are
 * __ccol_event_loop_destroy and the rollback of the constructor after a
 * failed slot acquire. A slot that the code marked as not in use makes every
 * further resolve of it fail. */
static ccol_retval_t _ccol_event_loop_shutdown_internal(
    struct ccol_event_loop_s *loop) {
  /* The self-call guard. A join of poller_thread below, made from
   * poller_thread itself, deadlocks exactly like a bare self-pthread_join. A
   * drain of dispatch_pool from inside one of its own worker threads
   * deadlocks in the same way. ctpool_shutdown_drain detects a worker of that
   * same pool that calls it, and it returns without a join, so the drain leg
   * is covered there too. This guard makes the poller-thread leg fail the
   * same way. The pool knows nothing about that thread. The guard gives both
   * legs one uniform answer.
   *
   * The code applies it uniformly to BOTH configurations. With
   * num_reactor_threads == 1, poller_thread is the only thread, and it is
   * trivially the one that runs whatever callback can call this. With
   * num_reactor_threads > 1, the caller can be any of the workers of
   * dispatch_pool. A guard on one configuration only would make the behaviour
   * of this function on misuse depend on how many reactor threads the caller
   * configured. That is a worse API than "always undefended" or "always
   * defended".
   *
   * The function returns ccol_not_permitted instead of a silent deadlock.
   * That enumerator already exists and fits exactly: the operation is not
   * allowed in the current state. The code reuses it and mints no new one.
   * See the hard rule of common.h on enumerator numbering.
   *
   * The guard identifies the poller and the dispatch workers by the mark
   * that each of them holds in ccol_event_loop_job_key_bundle, and never by
   * comparing ccol_get_thread_id() with poller_thread. Once poller_thread is
   * joined, glibc gives its thread ID to the next thread that starts, and an
   * unrelated thread that calls this after a shutdown would then be refused.
   * The mark holds the RAW pointer, and not any public handle value: a
   * struct ccol_event_loop_s*, not the uint64_t handle. */
  ccol_call_once(ccol_event_loop_job_key_bundle.once,
                 _ccol_event_loop_init_job_key);
  if (ccol_thread_ls_get(ccol_event_loop_job_key_bundle.key) == (void *)loop) {
    return ccol_not_permitted;
  }

  ccol_mutex_lock(loop->shutdown_lock);
  bool is_leader = !loop->shutdown_started;
  loop->shutdown_started = true;
  ccol_mutex_unlock(loop->shutdown_lock);

  if (is_leader) {
    atomic_store(&loop->shutting_down, true);

#if CCOL_FORK_SAFETY_REQUIRED
    bool is_foreign = atomic_load(&loop->foreign_since_fork);
#else
    bool is_foreign = false;
#endif
    if (is_foreign) {
      /* This process inherited `loop` across a fork() call. See the
       * comment of that field. poller_thread exists here only as inert,
       * copy-on-write memory. No execution context of its own ever existed in
       * this process. shutdown_efd is a real kernel object, and this process
       * still shares it with the genuinely live poller thread of the
       * still-running parent.
       *
       * The code therefore skips both steps below completely. A write to
       * shutdown_efd here would wrongly wake the poller of the PARENT. That
       * is a real, reproducible side effect and not a theoretical one,
       * because the eventfd object below it is the SAME one in both
       * processes. A ccol_thread_join(poller_thread) call would be undefined
       * behaviour. It reliably SIGSEGVs in the exactly analogous case of a
       * dispatch_pool worker. The foreign_since_fork field of
       * cthreadpool.c covers that half. ctpool_shutdown_drain below is
       * therefore already safe to call unconditionally, and this module needs
       * no fork awareness of its own for it. */
    } else {
      _eventfd_notify(loop->shutdown_efd);

      /* One write reliably wakes poller_thread. That holds for one reason.
       * The shared NULL-entry handling of _ccol_event_loop_handle_event and
       * _ccol_event_loop_poller_collect deliberately never drains
       * shutdown_efd. See the comment of _ccol_event_loop_handle_event
       * for the deadlock that such a drain causes. A reset of the counter to
       * 0 before epoll_wait can report it leaves the reactor stuck in
       * epoll_wait for ever, and this join hangs on it.
       *
       * The counter therefore stays above zero once the code writes it. The
       * epoll_wait call of poller_thread then keeps seeing shutdown_efd as
       * ready, because the registration is level-triggered. It keeps seeing
       * it until that call really returns. The shutting_down check of this
       * loop, which the code set above and before this write, then breaks it
       * out. The hang from a drain is real and not theoretical. The reactor
       * sits in this exact epoll_wait call with nothing left to report. The
       * multi-thread shutdown tests in tests/cthreadcomm/tests.c are the
       * regression coverage for this mechanism.
       *
       * The code joins poller_thread strictly before it drains
       * dispatch_pool. No further job can then ever reach the pool, because
       * only poller_thread ever calls ctpool_submit. That is what makes the
       * drain of dispatch_pool below safe. There is no separate race of a
       * submit after the start of the shutdown to handle. The documented
       * caller contract of ctpool_shutdown_drain says that no other thread
       * must submit tasks at the same time. The code satisfies that by
       * construction at the point of the call. */
      ccol_thread_join(loop->poller_thread);
    }

    /* The code drains the pool, and does not cancel it at once. That keeps
     * the documented contract of this function: no dispatch can be in
     * flight once this function returns. An immediate shutdown would cancel
     * the jobs that sit in the queue and never started, instead of a run of
     * them. It would leave their already-bumped reg and entry refcounts in a
     * state that nothing ever cleans up. dispatch_pool is NULL for
     * num_reactor_threads == 1, where the code created nothing to drain. The
     * call is safe unconditionally, even with foreign_since_fork. This is a
     * ctpool, and the identical fork fixup of cthreadpool.c already makes
     * ctpool_shutdown_drain join no thread in that case. See the
     * foreign_since_fork field of struct cthread_pool. */
    if (loop->dispatch_pool) {
      ctpool_shutdown_drain(loop->dispatch_pool);
    }

    ccol_mutex_lock(loop->shutdown_lock);
    loop->joined = true;
    ccol_cond_var_broadcast(loop->joined_cv);
    ccol_mutex_unlock(loop->shutdown_lock);
  } else {
    ccol_mutex_lock(loop->shutdown_lock);
    while (!loop->joined) {
      ccol_cond_var_wait(loop->joined_cv, loop->shutdown_lock);
    }
    ccol_mutex_unlock(loop->shutdown_lock);
  }

  return ccol_success;
}

ccol_retval_t ccol_event_loop_shutdown(ccol_event_loop loop) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = _ccol_event_loop_shutdown_internal(raw);
  _ccol_event_loop_resolve_unpin(raw);
  return rv;
}

/* Two callers share this helper. The first is __ccol_event_loop_destroy,
 * after its own poll-wait for pending_resolve_count == 0 finishes. The second
 * is the rollback of ccol_event_loop_create_with_mprocs after a failed slot
 * acquire. That caller makes no wait at all beforehand, because no handle
 * reached any caller at that point, so pending_resolve_count is provably
 * already 0.
 *
 * The helper runs the shutdown, when the code did not start it yet. It is
 * idempotent either way, through the leader and follower protocol of
 * _ccol_event_loop_shutdown_internal, which uses shutdown_lock,
 * shutdown_started and joined_cv. It then frees every resource that remains.
 *
 * Nothing calls this helper on a loop whose handle a caller can still
 * resolve. The teardown helper of ctpool is different. The
 * wait-ordering rule of ccol_event_loop says to wait BEFORE this runs, and
 * not after. See the comment of __ccol_event_loop_destroy. This helper
 * therefore never needs to wait on pending_resolve_count itself. */
static void _ccol_event_loop_teardown_raw(struct ccol_event_loop_s *loop) {
  _ccol_event_loop_shutdown_internal(loop);

  /* The code deferred some entries and regs during the final round of batch
   * processing, directly before it saw shutting_down. Those items never
   * reached a reclaim point on the schedule of their own defer_gen.
   * poller_thread and every dispatch_pool worker are joined now, so none of
   * them can access anything any more. It is therefore safe to free every
   * deferred item that remains, unconditionally, instead of a leak of them.
   * See the comment of _ccol_event_loop_free_all_pending for the reason
   * why no epoch check and no refcount check is needed at this exact
   * point. */
  _ccol_event_loop_free_all_pending(loop);
  _ccol_mem_free(loop->m_procs, loop->poller_events);

  /* The code joined poller_thread and every dispatch_pool worker. Every other
   * ccol_event_loop_shutdown caller that was in flight also returned by now.
   * The leader and follower join protocol above guarantees that. No dispatch
   * can be in flight, and no other thread can touch the registry of this
   * loop. It is therefore safe to walk and free every registration that
   * remains in every stripe, with no lock.
   *
   * The separate-chaining storage of chmap is packed. See _fd_registry_find.
   * The code therefore reads the stored event_entry* with a memcpy, and not
   * with a direct pointer cast. chashmap_begin_iter and it->_next_fn only
   * free the bookkeeping of the iterator as they walk. A free of what a
   * stored value POINTS TO is the responsibility of this loop. chmap_destroy
   * below is the same: it only frees the copies of the map of the int keys
   * and the pointer values. It never frees the event_entry structs that those
   * pointers name. */
  for (size_t i = 0; i < loop->num_stripes; i++) {
    ccol_event_loop_stripe_t *stripe = &loop->stripes[i];

    char *iter_err = NULL;
    cmap_iterator *it = chashmap_begin_iter(stripe->fd_index, &iter_err);
    while (it) {
      event_entry *entry;
      memcpy(&entry, it->val_pair->ptr, sizeof(entry));
      if (entry->as.fd.read_reg)
        _ccol_event_reg_free(loop, entry->as.fd.read_reg);
      if (entry->as.fd.write_reg)
        _ccol_event_reg_free(loop, entry->as.fd.write_reg);
      ccol_mutex_destroy(entry->dispatch_lock);
      _ccol_mem_free(loop->m_procs, entry);
      it = it->_next_fn(it);
    }
    chmap_destroy(stripe->fd_index);

    ccol_event_reg_s *reg = stripe->queue_regs_head;
    while (reg) {
      ccol_event_reg_s *next = reg->loop_list_next;
      ccol_mutex_t *q_mtx;
      ccol_sel_waiter **q_head;
      ccol_sel_waiter **q_rotor;
      _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
      /* The queue can have other listeners: a ccol_select() caller, or a
       * registration of another loop. A wake that this registration holds
       * and never acted on goes to the next of them, as in the queue branch
       * of _ccol_event_loop_remove_unlink. */
      ccol_mutex_lock(*q_mtx);
      _sel_unlink_waiter_locked(&reg->waiter_node, q_head, q_rotor);
      if (_queue_sel_is_ready(&reg->sel))
        notify_one_sel_waiter(q_head, q_rotor);
      ccol_mutex_unlock(*q_mtx);
      /* The code captures the entry and frees reg before the entry that reg
       * names. _ccol_event_reg_free frees reg, and it reads reg->owning_entry
       * first. That read decides whether this registration ever became live
       * enough for on_removed to fire. A free of the entry first leaves that
       * field with an indeterminate pointer value. C11 6.2.4p2 makes even an
       * evaluation of such a value undefined. That is true although the code
       * only tests whether it is non-NULL. Exactly this one registration owns
       * a queue-backed entry, so nothing else can see this order. */
      event_entry *owning = reg->owning_entry;
      _ccol_event_reg_free(loop, reg);
      ccol_mutex_destroy(owning->dispatch_lock);
      _ccol_mem_free(loop->m_procs, owning);
      reg = next;
    }

    ccol_mutex_destroy(stripe->lock);
  }
  _ccol_mem_free(loop->m_procs, loop->stripes);
  if (loop->dispatch_pool) __ctpool_destroy(loop->dispatch_pool);

  cvector_destroy(loop->reg_free_indices);
  cvector_destroy(loop->reg_slots);
  ccol_rw_lock_destroy(loop->reg_slot_rwlock);
  ccol_cond_var_destroy(loop->joined_cv);
  ccol_mutex_destroy(loop->shutdown_lock);
  close(loop->reclaim_wake_efd);
  close(loop->shutdown_efd);
  close(loop->epfd);

  if (loop->m_procs) {
    ccol_free_t free_func = loop->m_procs->free;
    free_func(loop->m_procs);
    free_func(loop);
  } else {
    ccol_mem_free(loop);
  }
}

void __ccol_event_loop_destroy(ccol_event_loop loop) {
  if (!loop) return;

  /* The code resolves loop through the slot table. It claims the teardown in
   * the same critical section as the lookup. A second destroy call on the
   * same handle value then fails the lookup. That call can be concurrent, or
   * it can come later. Either way it never races the teardown of this
   * call. See the file-level comment of the slot table
   * and the comment of _ccol_event_loop_resolve for the full design. A stale
   * or already-destroyed handle that reaches here is exactly the misuse that
   * the generation-checked handle design exists to catch. It is fatal, and
   * not a silent use-after-free or double free. */
  ccol_call_once(ccol_event_loop_slot_table.once,
                 _ccol_event_loop_slot_table_init_globals);
  ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  uint32_t idx = (uint32_t)(loop >> 32);
  uint32_t gen = (uint32_t)(loop & 0xFFFFFFFFu);
  ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  struct ccol_event_loop_s *raw = NULL;
  if (idx < _event_loop_slot_count_locked()) {
    ccol_event_loop_slot_t *s = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
    if (s->in_use && s->generation == gen && !s->ptr->destroy_claimed)
      raw = s->ptr;
  }
  if (!raw) {
    ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
    ccol_fatal_err(
        "ccol_event_loop_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a ccol_event_loop handle)");
  }

  /* The guard against a self-destroy from a callback. It is the destroy-side
   * analogue of the self-join guard of
   * _ccol_event_loop_shutdown_internal. __ccol_event_loop_destroy calls that
   * same guarded function internally, through _ccol_event_loop_teardown_raw.
   * But it cannot pass the ccol_not_permitted return of that function on. The
   * contract of this function returns void and comes from a macro.
   *
   * Without this check, a callback that destroys its own loop sees the
   * internal shutdown silently join nothing. This function still goes on and
   * frees every entry. That includes the one dispatch_lock that the calling
   * frame still holds locked. It also frees the loop struct itself. All of
   * that happens under the _ccol_event_loop_handle_event or
   * _ccol_event_loop_dispatch_job_fn frame that called it and still runs.
   * That is a heap use-after-free.
   *
   * The code checks here, before it claims the teardown. A
   * caller that misuses this from inside a callback therefore gets the same
   * loud, detected ccol_fatal_err() as every other genuinely fatal misuse of
   * this function. A stale or already-destroyed handle is such a misuse. The
   * caller does not get a silent skip followed by undefined behaviour. */
  ccol_call_once(ccol_event_loop_job_key_bundle.once,
                 _ccol_event_loop_init_job_key);
  bool is_self_call =
      (ccol_thread_ls_get(ccol_event_loop_job_key_bundle.key) == (void *)raw);
  if (is_self_call) {
    ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
    ccol_fatal_err(
        "ccol_event_loop_destroy: called from within a callback running on "
        "this "
        "loop's own reactor/dispatch thread (self-destroy hazard); defer "
        "destruction to another thread, or to after the callback returns, "
        "instead");
  }

  /* The claim makes this call the owner of the teardown. A second destroy
   * call, concurrent or later, reads it and fails as fatal above. The handle
   * itself still resolves, on purpose. The graceful shutdown below still
   * runs callbacks: poller_thread finishes its current batch, and
   * dispatch_pool drains every job that it queued. Those callbacks call
   * ccol_event_loop_remove, _pause, _resume, _modify and _add on this loop
   * exactly as they do during ccol_event_loop_shutdown. A removal that fails
   * there leaves the removed flag clear, so a second item that the same
   * batch collected for the same fd still runs with an arg that the first
   * callback already freed. */
  raw->destroy_claimed = true;
  ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);

  /* The leader and follower protocol of _ccol_event_loop_shutdown_internal
   * makes this call safe beside a concurrent ccol_event_loop_shutdown. The
   * self-call guard above already refused a call from a thread of this loop,
   * so this cannot return ccol_not_permitted. Once it returns, poller_thread
   * is joined and dispatch_pool is drained, so no callback runs any more. */
  _ccol_event_loop_shutdown_internal(raw);

  /* Only now does the handle stop resolving. The code fetches the slot by
   * idx under the lock, and keeps no pointer into the slot array across an
   * unlock: a concurrent ccol_event_loop_create_with_mprocs can reallocate
   * that array with cvector_push_back. */
  ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  ((ccol_event_loop_slot_t *)cvector_at(ccol_event_loop_slot_table.slots, idx))
      ->in_use = false;
  ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);

  /* The code waits for pending_resolve_count to reach 0 BEFORE it frees
   * anything. See the struct comment of this field for the reason why
   * ccol_event_loop is safe to wait first, where ctpool is not. Every public
   * entry point that holds a pin is a quick, bounded critical section that
   * takes a stripe lock only. Those entry points are ccol_event_loop_add,
   * _modify, _pause, _resume, _remove and _reg_count. None of them blocks on
   * the poller thread or on the broadcast machinery of the shutdown.
   *
   * This is a poll, and not a wait on a condition variable. See the field
   * comment of pending_resolve_count for the reason why that is correct and
   * carries no lost-wakeup risk. */
  while (atomic_load(&raw->pending_resolve_count) > 0) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000}; /* 100us */
    nanosleep(&ts, NULL);
  }

  _ccol_event_loop_teardown_raw(raw);

  /* The code releases the slot last, only after it fully tore raw down and
   * freed it. The generation bump of the slot, and the push-back of the free
   * index, are what mark the handle as reusable. No earlier step does that.
   *
   * The code fetches the slot again by idx. A
   * concurrent ccol_event_loop_create_with_mprocs can have run its own
   * _ccol_event_loop_handle_slot_acquire call in between. That call can have
   * reallocated the backing array of slots with cvector_push_back. Any
   * pointer into that array from before this second lock acquire is then
   * invalid. idx itself stays stable. */
  ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  ccol_event_loop_slot_t *slot2 = (ccol_event_loop_slot_t *)cvector_at(
      ccol_event_loop_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* This raises the generation of this slot past the
      value that the handle of the just-freed loop carried. That stale handle
      can then never again match the generation of a FUTURE acquire for this
      same index. */
  _ccol_event_loop_free_index_release_locked(idx);
  /* This is the last statement under this lock. The ptr of the slot is clear
     now, so this call performs the release that the process-exit destructor
     deferred. Nothing below reads the table. There is therefore no window in
     which the code takes the lock again and another destroy is caught. */
  _release_event_loop_slot_table_if_deferred_locked();
  ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
size_t ccol_event_loop_dispatch_pool_pending_count_for_tests(
    ccol_event_loop loop) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw || !raw->dispatch_pool) {
    if (raw) _ccol_event_loop_resolve_unpin(raw);
    return 0;
  }
  size_t n = ctpool_pending_count(raw->dispatch_pool);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}

/* Returns the thread ID of the poller thread of loop, joined or not. A test
 * starts threads until one carries this ID after a shutdown, to prove that
 * the self-call guards do not identify the poller by its ID. */
ccol_thread_id_t _ccol_event_loop_poller_thread_for_tests(
    ccol_event_loop loop) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  ccol_thread_id_t id;
  memset(&id, 0, sizeof(id));
  if (!raw) return id;
  id = raw->poller_thread;
  _ccol_event_loop_resolve_unpin(raw);
  return id;
}

/* Reads how many epoll_wait calls poller_thread completed so far. See the
 * poller_iterations_for_tests field comment of struct ccol_event_loop_s. A
 * test samples this twice across a short, bounded window. It then detects a
 * busy-spin directly, because the counter races ahead by a large amount. The
 * test needs no flaky measurement of the wall clock or of the CPU usage. This
 * function returns 0 for a loop handle that is invalid or stale. */
uint64_t ccol_event_loop_poller_iterations_for_tests(ccol_event_loop loop) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return 0;
  uint64_t n = atomic_load(&raw->poller_iterations_for_tests);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}

/* Reads how many event entries the epoch reclaim of loop freed so far. It
 * gives 0 for a loop handle that is not valid or that is stale. */
uint64_t _ccol_event_loop_entries_reclaimed_for_tests(ccol_event_loop loop) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return 0;
  uint64_t n = atomic_load_explicit(&raw->entries_reclaimed_for_tests,
                                    memory_order_relaxed);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}

/* Stores value into poller_batch_gen of loop. A test uses it to put the
 * counter just below a 32-bit boundary while the poller is idle, and then
 * checks that a deferred free past that boundary is still reclaimed. */
void _ccol_event_loop_set_poller_batch_gen_for_tests(ccol_event_loop loop,
                                                     uint64_t value) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return;
  atomic_store(&raw->poller_batch_gen, value);
  _ccol_event_loop_resolve_unpin(raw);
}

/* Resolves h to the struct ccol_event_loop_s* below it WITHOUT a pin. It does
 * not touch pending_resolve_count at all. It is a bare lookup in the slot
 * table. That is safe for a test for one reason. Test code that calls this
 * runs synchronously and on one thread, so there is no concurrent destroy to
 * race. The function _ccol_event_loop_resolve is different. There a test
 * must remember a matching _unpin call, and that is an easy gap to leave.
 * A forgotten unpin leaves pending_resolve_count permanently above zero on
 * that loop. Every future ccol_event_loop_destroy call against it then hangs
 * silently. This function returns NULL under exactly the same conditions as
 * _ccol_event_loop_resolve.
 */
struct ccol_event_loop_s *_ccol_event_loop_resolve_for_tests(
    ccol_event_loop h) {
  ccol_call_once(ccol_event_loop_slot_table.once,
                 _ccol_event_loop_slot_table_init_globals);
  ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol_rw_lock_rdlock(ccol_event_loop_slot_table.rwlock);
  struct ccol_event_loop_s *raw = NULL;
  if (idx < _event_loop_slot_count_locked()) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  return raw;
}

/* Reads how many slots the ccol_event_loop handle table holds now. The count
 * covers the slots that the table grew and the slots that the code freed and
 * did not reuse yet. A test can then assert one thing: a churn loop of
 * creates and destroys reuses freed slots. It does not grow the table without
 * bound. */
size_t _ccol_event_loop_slot_table_capacity_for_tests(void) {
  ccol_call_once(ccol_event_loop_slot_table.once,
                 _ccol_event_loop_slot_table_init_globals);
  ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  ccol_rw_lock_rdlock(ccol_event_loop_slot_table.rwlock);
  size_t n = _event_loop_slot_count_locked();
  ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  return n;
}

/* A test-only hook that builds a genuinely long-held pin. It resolves h
 * through the real _ccol_event_loop_resolve, so the pin is real. The bare
 * lookup of _ccol_event_loop_resolve_for_tests is different. The hook then
 * sleeps for ms milliseconds while it still holds the pin, and unpins after
 * that.
 *
 * Every real public entry point is quick and bounded. This module therefore
 * has no naturally slow call to prove that a concurrent destroy really blocks
 * on pending_resolve_count. Without this hook, such a test only shows that
 * the destroy happened not to crash. The hook gives the
 * resolve_then_use_race_destroy_waits test a reliable way to control that
 * directly. It returns false when h does not resolve at all, because there is
 * then nothing to hold a pin on. */
bool _ccol_event_loop_resolve_pin_and_sleep_for_tests(ccol_event_loop h,
                                                      int ms) {
  struct ccol_event_loop_s *raw = _ccol_event_loop_resolve(h);
  if (!raw) return false;
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
  nanosleep(&ts, NULL);
  _ccol_event_loop_resolve_unpin(raw);
  return true;
}

/* Test-only. These two functions lock and unlock the write side of the
 * rwlock of ccol_event_loop_slot_table directly. They go around every public
 * API function. They mirror two pairs. The first is the
 * ccol_event_loop_test_wrlock_reg_slot_for_tests and _wrunlock pair of this
 * same file, which covers the reg_slot_rwlock of one loop. The second is
 * the identical ctpool_test_wrlock_slot_table_for_tests and _wrunlock pair of
 * cthreadpool.c, which covers the process-wide slot table of that module.
 *
 * A test can hold this write side locked from a thread OTHER than the one
 * that will call fork(). It can hold it for a window of any length that it
 * controls precisely. That is the scenario that deterministically exercises
 * the TID-tracked write-lock reinit for ccol_event_loop_slot_table.rwlock in
 * the in_child branch of _cthreadcomm_atfork_release_impl.
 *
 * These functions deliberately have no "resolve" step. There is no
 * per-instance handle to resolve here, only the process-wide slot table
 * itself. The lock call therefore returns void, and the matching unlock call
 * takes no argument. That matches the analogous pair of cthreadpool, and
 * it differs from the reg_slot pair of this file. The AB-BA hazard of
 * the reg_slot pair does
 * not apply here. Neither call touches anything else that a concurrent
 * fork() can contend. */
void ccol_event_loop_test_wrlock_slot_table_for_tests(void) {
  ccol_call_once(ccol_event_loop_slot_table.once,
                 _ccol_event_loop_slot_table_init_globals);
  /* Every library path that takes this lock has registered the fork
   * handlers first; a test that takes it through this hook gets the same
   * state, whichever tests ran before it. */
  ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
}

void ccol_event_loop_test_wrunlock_slot_table_for_tests(void) {
  ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
}
#endif

/* The code frees the bookkeeping arrays of the slot table at process
 * exit. Without that, the --show-leak-kinds=all option of make memtest
 * reports them as still reachable. While a loop is still live, the release
 * is deferred to the destroy that frees the last slot; see
 * _cleanup_event_loop_slot_table. Once the table is released, a resolve
 * finds nothing and a create fails cleanly.
 *
 * The code MUST use ccol_call_once here. An __attribute__((destructor))
 * function runs unconditionally for the whole shared object, whichever parts
 * of it the process really used. Without the guard, a process that links this
 * library and never creates a single ccol_event_loop locks a mutex here that
 * nothing ever initialised. */
/* Answers whether any slot still names a loop. The caller holds the write
 * lock.
 *
 * The check reads slot->ptr, and not slot->in_use. A destroy clears in_use
 * once its shutdown has drained, so that a new resolve is rejected. The rest
 * of the teardown runs after that, and it clears ptr only in its final
 * locked step. A scan that trusted in_use would free this table out
 * from under a destroy that is still in that window. */
static bool _event_loop_any_slot_live_locked(void) {
  size_t slot_count = _event_loop_slot_count_locked();
  for (size_t i = 0; i < slot_count; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* The code uses cvector_destroy, and not __cvector_destroy. cvector_destroy
   sets the handle to NULL as it frees. A later call then answers "already
   released" here, instead of an index into a freed vector. The code
   deliberately does not destroy the rwlock. This function can run from an
   ordinary destroy that still holds that lock. */
static void _release_event_loop_slot_table_locked(void) {
  cvector_destroy(ccol_event_loop_slot_table.slots);
  cvector_destroy(ccol_event_loop_slot_table.free_indices);
  ccol_event_loop_slot_table.release_deferred = false;
  /* No loop is left, and none can be created from here on, so nothing sets
   * or reads the two thread-local keys of the dispatch again. A queue
   * destroy sees live false and does not read the marker key. A thread that
   * outlives a dlclose() never meets a key of this module, and the next load
   * of the library creates fresh ones. */
  if (atomic_exchange(&ccol_event_loop_job_key_bundle.live, false))
    ccol_thread_ls_key_delete(ccol_event_loop_job_key_bundle.key);
  if (atomic_exchange(&queue_dispatch_marker_bundle.live, false))
    ccol_thread_ls_key_delete(queue_dispatch_marker_bundle.key);
}

/* The code keeps this behind one out-of-line call. The destroy path that must
   make that call therefore keeps the code shape that it has without any of
   this. */
static __attribute__((noinline)) void
_release_event_loop_slot_table_if_deferred_locked(void) {
  if (ccol_event_loop_slot_table.release_deferred &&
      !_event_loop_any_slot_live_locked()) {
    _release_event_loop_slot_table_locked();
  }
}

__attribute__((destructor)) static void _cleanup_event_loop_slot_table(void) {
  ccol_call_once(ccol_event_loop_slot_table.once,
                 _ccol_event_loop_slot_table_init_globals);
  /* There is nothing left to free, and nothing safe to touch. The code sets
     the vectors to NULL as it destroys them, so a second run answers
     here. */
  if (!ccol_event_loop_slot_table.slots) return;
  ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  /* The release runs only once nothing can still resolve a handle. The order
     of the destructors between translation units is not this library's to
     decide. A later destructor can still hold a live loop, and it would
     otherwise find this table freed under it. The code therefore hands the
     release to whichever destroy frees the last slot, instead of a skip. A
     program that does destroy its loops then leaves nothing behind, in
     whichever order the destructors ran. */
  if (_event_loop_any_slot_live_locked()) {
    ccol_event_loop_slot_table.release_deferred = true;
    ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
    return;
  }
  _release_event_loop_slot_table_locked();
  ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
}
