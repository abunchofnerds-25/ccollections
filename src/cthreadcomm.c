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
#include <internal/catfork.h>
#include <internal/cdeadline.h>
#include <internal/chashinsert.h>
#include <internal/cpoll.h>
#include <internal/cqueueowner.h>
#include <internal/test_helpers/common.h>
#include <internal/test_helpers/cthreadcomm.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef RUNNING_UNIT_TESTS
#include <assert.h>
#endif

/* Initialises cond so that every absolute deadline handed to
 * _ccol_cond_var_timedwait on it is measured against CLOCK_MONOTONIC instead
 * of the default CLOCK_REALTIME. Returns 0 on success, or the first non-zero
 * status the threading library reports.
 *
 * Every condition variable in this file that a timed wait can block on is
 * initialised here, and every deadline for those waits comes from
 * CLOCK_MONOTONIC, so the two can't disagree about which clock a deadline is
 * on. CLOCK_REALTIME can be set by a user, and an administrator, an NTP step
 * or a resumed virtual machine can move it backwards at any moment. A
 * deadline on that clock then lies further in the future, and a
 * ccol_circq_timed_recv_zc with a 100 ms timeout blocks for the whole length
 * of the backward step. Nothing can step CLOCK_MONOTONIC, so the caller gets
 * the timeout it asked for. An untimed _ccol_cond_var_wait ignores the clock
 * attribute, so the untimed waiters pay nothing for this. */
static int _init_monotonic_cond_var(_ccol_cond_var_t *cond) {
  _ccol_cond_var_attr_t attr;
  int rv = _ccol_cond_var_attr_init(attr);
  if (rv != 0) return rv;
  rv = _ccol_cond_var_attr_setclock(attr, CLOCK_MONOTONIC);
  if (rv == 0) rv = _ccol_cond_var_init_ca(*cond, attr);
  _ccol_cond_var_attr_destroy(attr);
  return rv;
}

/* A waiter node, registered by a thread that blocks in ccol_select. Each node
 * lives on the heap for the whole call (on the stack a large n could
 * overflow it). Reading or changing the waiter list needs the owning queue's
 * mutex, and that's what keeps these nodes valid while a producer walks the
 * list. */
typedef struct ccol_sel_waiter {
  _ccol_mutex_t *sel_mtx;
  _ccol_cond_var_t *sel_cond;
  bool *ready;
  int efd; /* eventfd for epoll mode; -1 in condvar-only mode */
  /* How many listeners this node's outstanding wake has visited without any
   * of them moving a message. A wake delivered by a send, a receive or any
   * other fresh event carries 0. Only a ccol_event_loop registration reads
   * it, when it consumes its wake (see
   * _ccol_event_loop_queue_cascade_notify_next). Guarded by *sel_mtx. It
   * fits in the padding after efd, so the node doesn't grow. */
  uint32_t fwd_hops;
  struct ccol_sel_waiter *prev;
  struct ccol_sel_waiter *next;
} ccol_sel_waiter;

#ifdef RUNNING_UNIT_TESTS
/* Test-only. _sendto_cq, _recvfrom_cq, ccol_circq_disable_sending and
 * ccol_circq_enable_sending get here while holding the queue's mutex. The
 * delay stretches the window between that lock and the
 * _ccol_mutex_lock(*w->sel_mtx) a few lines below, normally only a few
 * instructions, to whatever the test needs, so the test can land a
 * concurrent fork()'s atfork prepare() handler inside it every time. That
 * reproduces the lock-order hazard between the queue mutex and wait_mtx
 * described in the doc comment of _cthreadcomm_atfork_prepare; without the
 * delay the test would depend on luck, since the production window is far
 * too narrow to hit by chance. See
 * fork_safety.fork_does_not_deadlock_with_queue_registered_before_event_loop
 * in tests/cthreadcomm/tests.c. */
static _Atomic int g_notify_waiter_test_delay_us = 0;
void _notify_waiter_test_set_delay_us(int us) {
  atomic_store(&g_notify_waiter_test_delay_us, us);
}

/* Test-only. Counts the eventfd wakes _notify_waiter delivers (see
 * _ccol_select_test_eventfd_wake_count in cthreadcomm.h). Relaxed ordering
 * is enough because a test reads it only after the threads bumping it have
 * finished the burst it measures. */
static _Atomic uint64_t g_notify_waiter_eventfd_wakes = 0;
uint64_t _ccol_select_test_eventfd_wake_count(void) {
  return atomic_load_explicit(&g_notify_waiter_eventfd_wakes,
                              memory_order_relaxed);
}
#endif

/* The wake descriptors used in this file; see ccol_wakefd_create() in
 * internal/cpoll.h. */
static void _eventfd_notify(int efd) { ccol_wakefd_notify(efd); }

static void _eventfd_drain(int efd) { ccol_wakefd_drain(efd); }

/* Wakes one waiter node. The caller holds the owning queue's mutex for the
 * whole call so the node pointer stays valid.
 *
 * *ready is the only record of whether a wake is outstanding for the node.
 * The first notify after the node's owner cleared it sets it and delivers
 * the wake, by writing to the node's eventfd when it has one and by
 * signalling its condition variable otherwise. A node with an eventfd never
 * waits on its condition variable, since a ccol_select() call in epoll mode
 * and a ccol_event_loop registration both wait on the eventfd. Every later
 * notify finds *ready already set and delivers nothing (the outstanding wake
 * covers it), so a queue with an event-loop listener costs one write(2) per
 * dispatch of that listener instead of one per message.
 *
 * The eventfd write happens under sel_mtx together with the store to
 * *ready, and the owner clears *ready and drains the eventfd in a single
 * critical section under the same mutex (see _queue_reg_consume_wake and the
 * Phase 3 step of ccol_select_timed). So the flag and the eventfd's counter
 * never disagree: *ready is true exactly when a wake is pending on the
 * eventfd, and a notify that lands after the clear always finds *ready false
 * and always writes, which means no wake gets lost.
 *
 * Every wake delivered here is a fresh one, so the node's hop count goes back
 * to 0 too (see the fwd_hops field). A fresh wake that merges into a
 * forwarded one restarts the walk that
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
  _ccol_mutex_lock(*w->sel_mtx);
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
  _ccol_mutex_unlock(*w->sel_mtx);
  if (!already_pending && w->efd < 0) _ccol_cond_var_signal(*w->sel_cond);
}

/* Delivers a wake exactly like _notify_waiter, but for a wake that a
 * listener forwards after a dispatch that moved nothing; hops is how many
 * listeners in a row the wake has visited without progress. If a wake is
 * already outstanding for w the smaller count wins, so a forward never cuts
 * short the walk of a wake that's already on its way. The caller holds the
 * owning queue's mutex. */
static void _notify_waiter_forwarded(ccol_sel_waiter *w, uint32_t hops) {
  _ccol_mutex_lock(*w->sel_mtx);
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
  _ccol_mutex_unlock(*w->sel_mtx);
  if (!already_pending && w->efd < 0) _ccol_cond_var_signal(*w->sel_cond);
}

/* True when the waiter list starting at head has more than k nodes. It stops
 * walking after k + 1 nodes. The caller holds the owning queue's mutex. */
static bool _sel_list_longer_than(const ccol_sel_waiter *head, uint32_t k) {
  uint32_t seen = 0;
  for (const ccol_sel_waiter *w = head; w != NULL; w = w->next) {
    if (seen == k) return true;
    seen++;
  }
  return false;
}

/* Wakes ONE waiter on a queue's read or write waiter list and moves *rotor
 * on, so the next call picks a DIFFERENT waiter and over time every linked
 * waiter gets its turn instead of the same one every time. We use it for
 * message and slot events, where exactly one resource became available and
 * waking several waiters would be a thundering herd. The caller holds the
 * queue's mutex.
 *
 * *rotor is either NULL ("start a fresh cycle at *head") or points to a node
 * linked in THIS exact list. Every unlink path keeps that true: when
 * _sel_unlink_waiter_locked removes the node that IS *rotor it moves *rotor
 * to that node's .next, or to NULL when there's no .next. Only this function
 * and that unlink fixup read *rotor directly.
 *
 * The rotation is what rules out silent, permanent starvation. Notifying
 * *head every time would work badly with the PERMANENTLY linked
 * registrations of ccol_event_loop. A ccol_select() caller's node is
 * transient: it links again each time the caller has to wait again, so its
 * position in the list changes. A registration's node doesn't, and since
 * _sel_link_waiter always prepends, the most recently linked registration
 * would stay *head* and be the ONLY one to get a direct notify for its whole
 * registered life.
 *
 * The "cascade" (_ccol_event_loop_queue_cascade_notify_next) can't replace
 * the rotation, because it forwards a wake along the list only when the
 * queue is STILL ready after the notified waiter's callback returns. A
 * callback that keeps up with the traffic often leaves nothing to forward,
 * and one plain ccol_circq_try_recv_zc() per call with no loop is exactly
 * such a callback, an ordinary documented pattern.
 *
 * Without the rotation one registration takes every notify and every OTHER
 * live registration on the same queue and direction gets no call at all for
 * as long as that one stays registered. Make two ccol_event_loop_add() calls
 * on the read direction of one ccol_circular_queue, let both callbacks do a
 * single ccol_circq_try_recv_zc() with no drain loop, and feed the queue one
 * message at a time so no backlog builds up: the second, older registration
 * then gets ZERO callbacks over thousands of messages. That breaks the
 * module's guarantee, stated in the doc comment of ccol_event_loop_add, that
 * no live listener is skipped indefinitely.
 *
 * The rotation changes the target waiter on every notify and works on its
 * own, next to the cascade. The cascade depends on readiness and has a
 * different job, surfacing a backlog that the target of a single notify can
 * leave behind. Since the rotation visits every live waiter on the list once
 * per full cycle, a sibling registration's callback can't keep the queue
 * drained and skip a waiter for ever. */
static void notify_one_sel_waiter(ccol_sel_waiter **head,
                                  ccol_sel_waiter **rotor) {
  ccol_sel_waiter *target = *rotor ? *rotor : *head;
  if (!target) return;
  _notify_waiter(target);
  /* When the rotation walks off the tail (target->next == NULL) this wraps
   * to NULL and the next call starts a fresh cycle at *head. *head may be a
   * different node than when this cycle started, since new waiters prepend
   * there, and that's fine: a full cycle only needs to reach every waiter
   * linked NOW, not keep the same walk order from one cycle to the next. */
  *rotor = target->next;
}

/* Wakes every thread blocked in ccol_select on this queue. We use it only for
 * the state changes disable_sending and enable_sending, after which every
 * blocked thread must look at the state again even if no resource became
 * available. The caller holds the queue's mutex. */
static void notify_all_sel_waiters(ccol_sel_waiter *head) {
  for (ccol_sel_waiter *w = head; w != NULL; w = w->next) _notify_waiter(w);
}

/* ========================================================================== */
/*         QUEUE DISPATCH REFERENCES (queue teardown vs. a live callback)     */
/* ========================================================================== */

/* A process-wide thread-local key, created on first use. It holds the queue
 * whose ccol_event_loop callback is running on the calling thread right now,
 * or NULL when none is. It's one key for the whole process rather than one
 * per queue for the same reason as ccol_event_loop's dispatch-job key
 * (declared further below): a single key answers "which queue's callback is
 * running on me right now" for every queue at once, with no per-queue
 * lifecycle to manage.
 *
 * live says whether the key exists. Creating it can fail, for example in a
 * process that already holds PTHREAD_KEYS_MAX keys, and key then names no
 * key of ours and may name a key some other component owns, so a value set
 * on it would end up in that component's destructor. Only a ccol_event_loop
 * dispatch sets the key, and ccol_event_loop_create_mp refuses to build a
 * loop while live is false, so nothing ever sets a key that doesn't exist. A
 * queue destroy reads it only while live is true. The key is deleted when
 * ccol_event_loop's slot table is released, which happens only once no loop
 * is left (see _release_event_loop_slot_table_locked), so repeated dlopen()
 * and dlclose() cycles don't use up the process's keys. */
static struct {
  _ccol_thread_ls_key_t key;
  _ccol_once_flag_t once;
  atomic_bool live;
} queue_dispatch_marker_bundle = {.once = _CCOL_ONCE_INIT};

static void _queue_dispatch_marker_init(void) {
  if (_ccol_thread_ls_key_create(queue_dispatch_marker_bundle.key, NULL) == 0)
    atomic_store(&queue_dispatch_marker_bundle.live, true);
}

/* True when the marker key exists. ccol_event_loop_create_mp checks this
 * before building a loop. */
static bool _queue_dispatch_marker_available(void) {
  _ccol_call_once(queue_dispatch_marker_bundle.once,
                  _queue_dispatch_marker_init);
  return atomic_load(&queue_dispatch_marker_bundle.live);
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. How many of this module's upcoming thread marks (the queue
 * dispatch marker and the poller and dispatch-job marks) get a
 * pthread_setspecific() that fails as it would without memory; see
 * _ccol_event_loop_test_fail_next_marks. */
static _Atomic uint32_t g_mark_set_failures = 0;

void _ccol_event_loop_test_fail_next_marks(uint32_t count) {
  atomic_store(&g_mark_set_failures, count);
}
#endif

/* Sets key's value for the calling thread and returns 0 or an error number.
 * Storing can fail when the C library has to allocate the thread's storage
 * for the key and can't. */
static int _cthreadcomm_mark_set(_ccol_thread_ls_key_t key, void *value) {
#ifdef RUNNING_UNIT_TESTS
  uint32_t left = atomic_load(&g_mark_set_failures);
  while (left > 0) {
    if (atomic_compare_exchange_weak(&g_mark_set_failures, &left, left - 1))
      return ENOMEM;
  }
#endif
  return _ccol_thread_ls_set(key, value);
}

/* The mark of a thread whose key couldn't hold it. Each of our marks tries
 * its key first and writes this copy only when the key fails, the thread
 * clears the copy itself before the mark ends, and a check reads the key and
 * then this copy. Without it a failed key store would leave the thread
 * unmarked, and a destroy from inside a callback would then wait for itself
 * instead of being reported. */
static __thread const void *_queue_dispatch_mark_fallback;

/* Marks the calling thread as running queue's ccol_event_loop callback
 * (queue is not NULL). Only a live loop's dispatch calls this, and a loop
 * exists only once _queue_dispatch_marker_available() said true, so the key
 * exists here and the dispatch path pays for no extra test. Returns true when
 * the key couldn't hold the mark and the thread-local copy holds it instead;
 * the caller passes that answer on to _queue_dispatch_unmark. */
static bool _queue_dispatch_mark(void *queue) {
  _ccol_call_once(queue_dispatch_marker_bundle.once,
                  _queue_dispatch_marker_init);
  if (_cthreadcomm_mark_set(queue_dispatch_marker_bundle.key, queue) == 0)
    return false;
  _queue_dispatch_mark_fallback = queue;
  return true;
}

/* Clears what _queue_dispatch_mark set. A key that held the mark gets NULL,
 * which needs no new storage and so can't fail. */
static void _queue_dispatch_unmark(bool fell_back) {
  if (fell_back)
    _queue_dispatch_mark_fallback = NULL;
  else
    (void)_ccol_thread_ls_set(queue_dispatch_marker_bundle.key, NULL);
}

/* Without the key no loop can exist, so no queue's callback can be running on
 * this thread. */
static bool _queue_dispatch_marker_is(const void *queue) {
  if (!_queue_dispatch_marker_available()) return false;
  return _ccol_thread_ls_get(queue_dispatch_marker_bundle.key) == queue ||
         _queue_dispatch_mark_fallback == queue;
}

#ifdef RUNNING_UNIT_TESTS
bool _ccol_event_loop_test_dispatches_queue(const void *queue) {
  return _queue_dispatch_marker_is(queue);
}
#endif

/* Blocks until every ccol_event_loop dispatch holding a reference to this
 * queue is done with it. A queue's destroy function calls this once the
 * caller has drained the queue and removed every registration watching it.
 *
 * ccol_event_loop_remove doesn't wait for a callback the reactor collected
 * before it ran, because waiting there would be an AB-BA deadlock against
 * any application lock a caller might hold across the removal; that's why
 * the removal is asynchronous and reports completion through on_removed.
 * Unlinking the registration's waiter node makes the queue look unwatched
 * the moment remove returns, but a dispatch that has already started its
 * callback holds the queue through its own collected snapshot and is about
 * to call into it. Only such a dispatch holds a reference. One still waiting
 * in the dispatch pool's queue holds none and never touches the queue after
 * the removal (see _queue_dispatch_ref_acquire). Without this wait, freeing
 * the queue at that point is a use-after-free: the callback's
 * ccol_circq_try_recv_zc reads a freed queue, or the reactor blocks for ever
 * locking a destroyed mutex, which hangs the poller thread and every later
 * ccol_event_loop_destroy that joins it.
 *
 * This wait isn't the thing remove refuses to do. The function holds no lock
 * at all (the queue's mutex is unlocked before we get here), so it can't be
 * one side of a lock-order cycle. The dispatch it waits on takes four locks,
 * the entry's dispatch lock, the loop's stripe lock, the queue's mutex and
 * the registration's wait mutex, and this thread holds none of them. There
 * is one way a caller can still block itself that we don't catch: holding,
 * across the destroy, an application lock the in-flight callback also needs,
 * which every destroy function's documentation forbids.
 *
 * A destroy from inside that same callback would wait on itself for ever, so
 * the thread-local marker above detects that case and makes it fatal, just
 * like a thread pool refuses a worker that destroys its own pool. No resolve
 * or pin scheme can catch it, since the callback reaches the queue through
 * its own call stack and not through a resolve.
 *
 * We poll here instead of waiting for a signal. A signal would cost the
 * dispatch path one condition variable signal per callback, paid for a wait
 * that is cold by design. The poll contends on nothing while it sleeps: it
 * reads one atomic and holds no lock, so unlike a retry loop that keeps
 * taking a lock it can't slow down the dispatch it is waiting for. */
static void _queue_wait_for_dispatch_refs(_Atomic size_t *refs,
                                          const void *queue) {
  if (atomic_load(refs) == 0) return;

  if (_queue_dispatch_marker_is(queue)) {
    _ccol_fatal_err(
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

/* A process-wide registry of the mutex address (_ccol_mutex_t*) of every
 * live ccol_circular_queue and ccol_dynamic_queue. On first use we register
 * a _ccol_at_fork() handler that locks all of these mutexes before fork()
 * proceeds and unlocks them right after it, in the parent and in the child.
 *
 * fork() copies only the calling thread, but some OTHER thread may hold a
 * lock at that moment, here a cq->mutex or a dq->mutex taken in the middle of
 * a send or a receive. The child inherits that lock locked, with no thread
 * left that could ever unlock it, so every later operation on the same queue
 * in the child hangs for ever. ccol_circq_send_zc, ccol_circq_recv_zc and
 * ccol_dynmq_send_zc are such operations, since each starts with
 * _ccol_mutex_lock(cq->mutex) or _ccol_mutex_lock(dq->mutex).
 * ccol_event_loop's atfork handling closes this hazard for every lock IT
 * owns, which includes a queue-backed registration's wait_mtx, and this
 * registry closes the same gap for the queue's own mutex. wait_mtx can't
 * stand in for it, because wait_mtx only guards _notify_waiter()'s generic
 * ready-flag handshake, while msg_count, writing_disabled and the waiter
 * lists all sit behind cq->mutex or dq->mutex, and no lock ccol_event_loop
 * owns covers those, whether or not the queue is registered with a loop.
 *
 * ccol_channel needs no entry of its own. It is backed by two
 * ccol_circular_queue instances that ccol_circular_queue_create_mp registers
 * here, so both directions of a channel are covered automatically.
 *
 * Unlike ccol_event_loop_slot_table this registry has no generations or
 * handles to maintain, because a ccol_circular_queue and a
 * ccol_dynamic_queue are plain pointers, not opaque handles. It's just an
 * unordered bag of live _ccol_mutex_t* addresses: an address goes in at
 * construction and comes out at destruction, only so the merged atfork
 * handler has something to walk (see _cthreadcomm_atfork_prepare, defined
 * further below after the full declaration of struct _ccol_event_loop_s).
 * The registry needs no child-only fixup, while ccol_event_loop's
 * child-release path does (it marks foreign_since_fork, replaces its epfd
 * and so on). A ccol_circular_queue or ccol_dynamic_queue owns no kernel
 * object and no thread, so once the mutex is properly unlocked after the
 * fork the whole queue state (msg_count, the message array and the waiter
 * lists) is exactly what it was at the fork, and the child can use it as
 * safely as the parent. The same goes for every plain condition variable of
 * this module, read_cond and write_cond here or ccol_event_loop's joined_cv
 * for example. Only a LOCK held by a thread that doesn't exist in the child
 * is a hazard, and a condition variable is never "locked", so a fork() needs
 * no work on one.
 *
 * IMPORTANT: this registry does NOT get its own independent _ccol_at_fork()
 * call, because a second, completely separate _ccol_at_fork() triple next to
 * ccol_event_loop's is a real deadlock. pthread_atfork runs prepare handlers
 * in REVERSE registration order, so which of two independent handler sets
 * runs first is an accident of which subsystem the process happened to use
 * first, a ccol_circular_queue or a ccol_event_loop. Say ccol_event_loop's
 * prepare runs first and locks the reg->wait_mtx of a queue-backed
 * registration. A separate registry prepare running second then locks the
 * cq->mutex of that SAME queue, the reverse of the order every ordinary code
 * path uses: _notify_waiter always locks cq->mutex first and reg->wait_mtx
 * second, and ccol_circq_send_zc, ccol_circq_recv_zc and the like call
 * _notify_waiter from a completely unrelated thread running at the same
 * time. That's a textbook AB-BA cycle. Create a queue before the process's
 * first ccol_event_loop, register it with a loop, and let a real
 * ccol_circq_send_zc race a fork(): the forking thread blocks inside fork()
 * while the registry's prepare handler locks cq->mutex, and at the same
 * moment the sending thread blocks on reg->wait_mtx inside _notify_waiter.
 *
 * So the locking of both subsystems is merged into ONE triple,
 * _cthreadcomm_atfork_prepare, _release and _child_release, defined further
 * below and registered by a single _ccol_at_fork() call. It locks each
 * queue's mutex at the one position that agrees with EVERY real nested-lock
 * pattern in this file: after the stripe lock owning any of the queue's
 * ccol_event_loop registrations (matching the stripe->lock before cq->mutex
 * nesting of _ccol_event_loop_add_queue and _ccol_event_loop_remove_unlink),
 * and before any of those registrations' wait_mtx (matching the cq->mutex
 * before wait_mtx nesting of _notify_waiter). The comment of that function
 * describes the three-phase design. The registry's struct and its add and
 * remove functions live here, next to the ccol_circular_queue and
 * ccol_dynamic_queue code, while the fork locking logic sits next to the
 * ccol_event_loop code because it needs the full definition of struct
 * _ccol_event_loop_s. */
#if CCOL_FORK_SAFETY_REQUIRED
/* One queue_mutex_registry entry: a live queue's mutex, plus the address of
 * that queue's registry_idx field, which always holds this entry's position
 * in the vector. */
typedef struct {
  _ccol_mutex_t *mutex;
  size_t *index;
} _queue_mutex_registry_entry;

static struct {
  _ccol_mutex_t mutex;
  _ccol_once_flag_t once;
  /* An unordered cvec of _queue_mutex_registry_entry. A remove gets its
   * entry's position from the queue and moves the last entry into the hole,
   * so it costs O(1) however many queues are live. NULL once the registry is
   * released at process exit (see _cleanup_queue_mutex_registry). */
  cvec addrs;
  /* Set when the process-exit destructor finds a live queue and leaves the
   * vector alone; the remove that later takes out the last address then does
   * the release the destructor couldn't. The order of destructors across
   * translation units isn't ours to decide, and an application destructor
   * can destroy a queue after this one has run. */
  bool release_deferred;
} queue_mutex_registry = {0};
#endif

/* Guards the ONE shared _ccol_at_fork() registration that covers both this
 * registry's queue mutexes and ccol_event_loop_slot_table's locks (the doc
 * comment of queue_mutex_registry explains why it's one merged registration
 * and not two). The flag lives here and not only next to
 * ccol_event_loop_slot_table so that _queue_mutex_registry_add and
 * _queue_mutex_registry_remove can trigger the registration by themselves:
 * a process can reach them without ever touching ccol_event_loop, so they
 * can't depend on ccol_event_loop_slot_table existing first. */
static _ccol_once_flag_t g_cthreadcomm_atfork_once = _CCOL_ONCE_INIT;

/* Forward declaration of the function that registers the merged
 * _ccol_at_fork() triple. Its body must come after the full declaration of
 * struct _ccol_event_loop_s further below, because the merged prepare and
 * release functions it wires up dereference loop->shutdown_lock,
 * loop->reg_slot_rwlock and loop->stripes[]. On first use it also
 * initialises the plain data of ccol_event_loop_slot_table (the mutex and
 * both cvecs) through that table's _ccol_call_once, and its caller
 * initialises queue_mutex_registry's plain data on first use the same way.
 * So BOTH structures are fully ready before the shared _ccol_at_fork()
 * handlers can run, whichever subsystem the process uses first. */
static void _cthreadcomm_register_atfork_once(void);

#if CCOL_FORK_SAFETY_REQUIRED
static void _queue_mutex_registry_init_globals(void) {
  if (_ccol_mutex_init(queue_mutex_registry.mutex) != 0)
    _ccol_fatal_err("queue mutex registry: failed to initialize mutex");
  queue_mutex_registry.addrs =
      cvector_create(sizeof(_queue_mutex_registry_entry), NULL);
  if (!queue_mutex_registry.addrs)
    _ccol_fatal_err("queue mutex registry: failed to allocate address vector");
}

/* Registers m so that a later fork() locks it in _cthreadcomm_atfork_prepare.
 * index is the registry_idx field of the queue that owns m, where the
 * registry keeps the entry's position (under the registry mutex) for
 * _queue_mutex_registry_remove. It's called once per live queue, right after
 * that queue's mutex is initialised and every other field is fully built, so
 * a failure here unwinds like any other allocation failure in the same
 * constructor. Returns false on failure, and the only failure is this
 * registry's cvector_push_back not being able to grow the vector. The caller
 * treats that like any other allocation failure at construction time: tear
 * down what was built and report an ordinary, graceful error, never abort
 * the process. For the queue's constructor it's just an out-of-memory case. */
static bool _queue_mutex_registry_add(_ccol_mutex_t *m, size_t *index) {
  _ccol_call_once(queue_mutex_registry.once,
                  _queue_mutex_registry_init_globals);
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  _ccol_mutex_lock(queue_mutex_registry.mutex);
  /* A NULL vector means the process-exit destructor released the registry. A
   * queue built after that can't be made fork-safe, so its construction
   * fails as if an allocation had failed. */
  bool ok = false;
  if (queue_mutex_registry.addrs != NULL) {
    _queue_mutex_registry_entry e = {.mutex = m, .index = index};
    *index = cvector_elem_count(queue_mutex_registry.addrs);
    ok = cvector_push_back(queue_mutex_registry.addrs, &e) == ccol_success;
  }
  _ccol_mutex_unlock(queue_mutex_registry.mutex);
  return ok;
}

#ifdef RUNNING_UNIT_TESTS
/* Counts the registry entries that _queue_mutex_registry_remove examines. */
static _Atomic uint64_t g_queue_registry_remove_probes_for_tests = 0;
#endif

/* Unregisters m, which an earlier _queue_mutex_registry_add(m, index) with
 * the same index added. It reads the entry's position from *index and moves
 * the registry's current last entry into the hole (updating the
 * registry_idx of the queue owning the moved entry) instead of shifting all
 * later entries down, since the registry is an unordered bag and
 * _cthreadcomm_atfork_prepare and _release promise nothing about walk order.
 * If the entry at *index isn't m it does nothing. That check is only a
 * defence, because every real caller removes, exactly once, an address it
 * added itself successfully. */
static void _queue_mutex_registry_remove(_ccol_mutex_t *m, size_t *index) {
  _ccol_call_once(queue_mutex_registry.once,
                  _queue_mutex_registry_init_globals);
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  _ccol_mutex_lock(queue_mutex_registry.mutex);
  if (!queue_mutex_registry.addrs) {
    _ccol_mutex_unlock(queue_mutex_registry.mutex);
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
  /* Last statement under this lock: once the last live queue is gone, do the
   * release the process-exit destructor deferred. */
  if (queue_mutex_registry.release_deferred &&
      cvector_elem_count(queue_mutex_registry.addrs) == 0) {
    cvector_destroy(queue_mutex_registry.addrs);
    queue_mutex_registry.release_deferred = false;
  }
  _ccol_mutex_unlock(queue_mutex_registry.mutex);
}

#ifdef RUNNING_UNIT_TESTS
/* Puts the number of registered queue mutexes in *count and the number of
 * entries removes have examined so far in *remove_probes. Returns true when
 * every entry's queue has a registry_idx naming that entry's position. */
bool _ccol_queue_mutex_registry_check_for_tests(size_t *count,
                                                uint64_t *remove_probes) {
  _ccol_call_once(queue_mutex_registry.once,
                  _queue_mutex_registry_init_globals);
  _ccol_mutex_lock(queue_mutex_registry.mutex);
  size_t n = queue_mutex_registry.addrs
                 ? cvector_elem_count(queue_mutex_registry.addrs)
                 : 0;
  bool ok = true;
  for (size_t k = 0; k < n; k++) {
    _queue_mutex_registry_entry *e = (_queue_mutex_registry_entry *)cvector_at(
        queue_mutex_registry.addrs, k);
    if (*e->index != k) ok = false;
  }
  _ccol_mutex_unlock(queue_mutex_registry.mutex);
  if (count) *count = n;
  if (remove_probes)
    *remove_probes = atomic_load(&g_queue_registry_remove_probes_for_tests);
  return ok;
}
#endif

/* Frees the registry's backing array at process exit, like
 * _cleanup_event_loop_slot_table does; without it, make memtest's
 * --show-leak-kinds=all reports the array as still reachable. While a queue
 * is still registered it only sets release_deferred, and removing the last
 * address frees the vector, so a queue that an application destructor
 * destroys after this one ran still finds the registry intact.
 * cvector_destroy sets the handle to NULL, so a later add or remove sees the
 * release and not a freed vector. The mutex stays, since the merged fork
 * handlers can still take it. */
__attribute__((destructor)) static void _cleanup_queue_mutex_registry(void) {
  _ccol_call_once(queue_mutex_registry.once,
                  _queue_mutex_registry_init_globals);
  _ccol_mutex_lock(queue_mutex_registry.mutex);
  if (queue_mutex_registry.addrs) {
    if (cvector_elem_count(queue_mutex_registry.addrs) > 0)
      queue_mutex_registry.release_deferred = true;
    else
      cvector_destroy(queue_mutex_registry.addrs);
  }
  _ccol_mutex_unlock(queue_mutex_registry.mutex);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* Puts the error a condition-variable wait returned into errno. The wait
 * returns its error and leaves errno alone, but the timed send and receive
 * functions document that errno holds the cause of ccol_unexpected_failure.
 * It's called before unlocking the queue mutex, which never changes errno.
 * Keeping it out of line means the timed functions don't need a callee-saved
 * register for the error code, so their common path keeps its shape. */
static __attribute__((noinline, cold)) void _ccol_store_wait_errno(int err) {
  errno = err;
}

struct ccol_circular_queue {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t read_cond;
  _ccol_cond_var_t write_cond;

  /* How many threads are blocked on the condition variables above. The
   * waiters keep these counts themselves while holding the mutex, and we
   * signal only when the relevant count isn't zero.
   *
   * It's about throughput, not correctness. A signal with no waiter still
   * costs a call into the threading library and a read of the condition
   * variable's shared state, which its waiters write, and the gate takes both
   * off the common path of every send and receive, on top of the mutex those
   * callers already share. It must never hide a real waiter, which is why
   * every wait site, timed ones included, sits between the increment and the
   * decrement of these counts. */
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

  /* Round-robin cursors for notify_one_sel_waiter (see its doc comment).
   * NULL means "start a fresh cycle at the matching head". */
  ccol_sel_waiter *sel_read_rotor;
  ccol_sel_waiter *sel_write_rotor;
  /* Registrations of shut-down ccol_event_loop instances that still name this
   * queue although their waiter nodes have left the lists above (see
   * _ccol_event_loop_detach_queue_regs). A destroy refuses the queue while
   * this isn't zero, just as while a waiter is linked. Guarded by mutex. */
  size_t detached_loop_regs;

  /* How many ccol_event_loop dispatches hold a reference to this queue right
   * now. A dispatch takes one when it starts a callback whose selectable
   * names this queue (on the dispatch-pool path, when the job starts; see
   * _queue_dispatch_ref_acquire) and drops it once the callback and its
   * cascade step have both finished. __ccol_circular_queue_destroy waits for
   * the count to reach zero before freeing anything (see
   * _queue_wait_for_dispatch_refs). It is deliberately the last of these
   * fields, well away from the ones the send and receive paths read, and
   * neither path touches it. */
  _Atomic size_t dispatch_refs;

#if CCOL_FORK_SAFETY_REQUIRED
  /* This queue's position in queue_mutex_registry. Only the registry reads or
   * writes it, under the registry mutex, and it sits after every field the
   * send and receive paths use. */
  size_t registry_idx;
#endif
};

/* Checks the arguments for creating a ccol_circular_queue: max_size must be
 * positive and at most ccol_max_elem_count, and a custom allocator, if one is
 * given, must be well formed.
 */
static bool verify_circular_queue_create_inputs(
    size_t max_size, ccol_memmgmt_procs_t *mmgmt_procs, char **err_str) {
  if (max_size == 0) {
    if (err_str) {
      *err_str = _CCOL_ERR_STR("max_size should be positive");
    }
    return false;
  }

  if (max_size > ccol_max_elem_count) {
    if (err_str) {
      *err_str = _CCOL_ERR_STR("max_size can not exceed ccol_max_elem_count");
    }
    return false;
  }

  /* ccol_circular_queue_create_mp backs the queue with one array of
   * max_size * sizeof(c_message_t) bytes, computed with a plain
   * multiplication and allocated without an overflow-checking allocator such
   * as calloc. Without this guard a max_size of roughly
   * [ccol_max_elem_count / 16, ccol_max_elem_count], the top slice of what
   * the checks above accept, wraps that multiplication around size_t. The
   * allocator then sees a tiny request, or even zero bytes, while the queue
   * thinks it has room for max_size messages, and the first send corrupts the
   * heap. cvector.c, csort.c and cmempool.c use the same SIZE_MAX /
   * element_size guard for the same kind of allocation. */
  if (max_size > SIZE_MAX / sizeof(c_message_t)) {
    if (err_str) {
      *err_str = _CCOL_ERR_STR(
          "max_size is too large: max_size * sizeof(c_message_t) would "
          "overflow size_t");
    }
    return false;
  }

  if (!_ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return false;
  }

  return true;
}

/* Allocates and initialises a bounded circular queue with its fixed-size
 * message array, its mutex and both condition variables. Sending starts out
 * turned on. */
ccol_circular_queue *ccol_circular_queue_create_mp(
    size_t max_size, ccol_memmgmt_procs_t *mmgmt_procs, char **err_str) {
  if (!verify_circular_queue_create_inputs(max_size, mmgmt_procs, err_str)) {
    return NULL;
  }

  ccol_circular_queue *cq = (ccol_circular_queue *)_ccol_mem_alloc(
      mmgmt_procs, sizeof(ccol_circular_queue));
  if (!cq) {
    if (err_str) {
      *err_str =
          _CCOL_ERR_STR("Failed to allocate memory for ccol_circular_queue");
    }
    return NULL;
  }

  if (!_ccol_populate_mem_mgmt_procs(cq, mmgmt_procs, err_str)) {
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }

  cq->msg_array = (c_message_t *)_ccol_mem_alloc(
      mmgmt_procs, max_size * sizeof(c_message_t));
  if (!cq->msg_array) {
    if (err_str) {
      *err_str = _CCOL_ERR_STR("Failed to allocate memory for cq msg_array");
    }
    _ccol_mem_free(mmgmt_procs, cq->m_procs);
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }

  if (_ccol_mutex_init(cq->mutex) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("Failed to initialize cq mutex");
    _ccol_mem_free(mmgmt_procs, cq->msg_array);
    _ccol_mem_free(mmgmt_procs, cq->m_procs);
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }
  if (_init_monotonic_cond_var(&cq->read_cond) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("Failed to initialize cq read_cond");
    _ccol_mutex_destroy(cq->mutex);
    _ccol_mem_free(mmgmt_procs, cq->msg_array);
    _ccol_mem_free(mmgmt_procs, cq->m_procs);
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }
  if (_init_monotonic_cond_var(&cq->write_cond) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("Failed to initialize cq write_cond");
    _ccol_mutex_destroy(cq->mutex);
    _ccol_cond_var_destroy(cq->read_cond);
    _ccol_mem_free(mmgmt_procs, cq->msg_array);
    _ccol_mem_free(mmgmt_procs, cq->m_procs);
    _ccol_mem_free(mmgmt_procs, cq);
    return NULL;
  }
  cq->read_index = 0;
  cq->write_index = 0;
  cq->max_size = max_size;
  cq->msg_count = 0;
  /* Set explicitly like every other field here, because the struct comes
   * from an allocation that doesn't zero memory and we signal only when one
   * of these fields isn't zero. Without it the common case is a stray
   * non-zero value that costs a signal nobody waits for, and the rare case is
   * a value whose increment wraps to zero, after which nothing ever wakes a
   * real waiter again. */
  cq->readers_waiting = 0;
  cq->writers_waiting = 0;
  cq->writing_disabled = false;
  cq->sel_read_waiters_head = NULL;
  cq->sel_write_waiters_head = NULL;
  cq->sel_read_rotor = NULL;
  cq->sel_write_rotor = NULL;
  cq->detached_loop_regs = 0;
  atomic_init(&cq->dispatch_refs, (size_t)0);

#if CCOL_FORK_SAFETY_REQUIRED
  /* Registering is the LAST step, once cq is fully built, so a failure here
   * unwinds like any earlier allocation failure in this function. The
   * comment of _queue_mutex_registry_add explains why this failure is
   * graceful and not fatal. */
  if (!_queue_mutex_registry_add(&cq->mutex, &cq->registry_idx)) {
    if (err_str) {
      *err_str = _CCOL_ERR_STR(
          "Failed to register ccol_circular_queue's mutex for fork safety");
    }
    _ccol_mutex_destroy(cq->mutex);
    _ccol_cond_var_destroy(cq->read_cond);
    _ccol_cond_var_destroy(cq->write_cond);
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

/* Destroys the circular queue. It asserts if a message nobody consumed is
 * still in the queue (its data pointer would leak), and if a ccol_select()
 * or ccol_event_loop waiter is still linked into one of the waiter lists.
 * The second case isn't just a leak but a real use-after-free hazard:
 * unlinking a node goes through the queue holding the list, so the
 * deregister step reads cq->sel_*_waiters_head, cq->sel_*_rotor and
 * cq->mutex. The caller must call ccol_event_loop_remove() first, or let
 * ccol_select() return first. Destroy cq before that and the next touch of
 * the node, from ccol_event_loop_remove, from __ccol_event_loop_destroy's
 * teardown walk or from ccol_select_timed's Phase 3 deregister, reads freed
 * queue memory. Both are caller bugs, and we make them visible
 * before freeing the memory so they don't show up later as corruption. */
void __ccol_circular_queue_destroy(ccol_circular_queue *cq) {
  if (cq) {
    /* Read under the mutex, because Phase 1 or Phase 3 of a concurrent
     * ccol_select() call, or a producer's or consumer's notify_one_sel_waiter
     * call, can link or unlink a node in either list at this very moment, so
     * an unlocked peek at fields another thread updates isn't race-free. */
    _ccol_mutex_lock(cq->mutex);
    bool has_sel_waiters = (cq->sel_read_waiters_head != NULL) ||
                           (cq->sel_write_waiters_head != NULL) ||
                           cq->detached_loop_regs != 0;
    _ccol_mutex_unlock(cq->mutex);
    if (has_sel_waiters) {
      _ccol_assert(false);
    }

    /* An empty waiter list alone doesn't mean nothing uses this queue.
     * The reactor can collect a callback before ccol_event_loop_remove runs,
     * and that callback holds the queue through its own snapshot and is
     * about to call into it. See _queue_wait_for_dispatch_refs. */
    _queue_wait_for_dispatch_refs(&cq->dispatch_refs, cq);

    /* We read the count only once no callback can touch the queue any more.
     * A write callback that was running when its registration was removed can
     * still send after the removal, and a count taken before the wait would
     * miss that message, whose data pointer would then disappear with the
     * queue without a trace. */
    if (ccol_circq_msg_count(cq) > 0) {
      _ccol_assert(false);
    }

    if (cq->msg_array) {
      _ccol_mem_free(cq->m_procs, cq->msg_array);
      cq->msg_array = NULL;
    }

#if CCOL_FORK_SAFETY_REQUIRED
    /* Unregister the mutex before destroying it, so that once it's destroyed
     * &cq->mutex is never again something _cthreadcomm_atfork_prepare can
     * lock. */
    _queue_mutex_registry_remove(&cq->mutex, &cq->registry_idx);
#endif

    _ccol_mutex_destroy(cq->mutex);
    _ccol_cond_var_destroy(cq->read_cond);
    _ccol_cond_var_destroy(cq->write_cond);

    if (cq->m_procs) {
      ccol_free_t free_func = cq->m_procs->free;
      free_func(cq->m_procs);
      free_func(cq);
    } else {
      _ccol_default_mem_free(cq);
    }
  }
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. Lock and unlock cq's internal mutex directly, bypassing every
 * public API function, so a test can hold the mutex for as long as it likes.
 * See their doc comments in cthreadcomm.h. */
void _ccol_circq_test_lock_mutex_for_tests(ccol_circular_queue *cq) {
  _ccol_mutex_lock(cq->mutex);
}

void _ccol_circq_test_unlock_mutex_for_tests(ccol_circular_queue *cq) {
  _ccol_mutex_unlock(cq->mutex);
}

/* Test-only. Reports whether a ccol_select() caller is linked into cq's
 * read-waiter list right now. It does its own lock, check and unlock instead
 * of being built from the two functions above, because a test composing
 * those would hold the mutex across its own check with no bound, which is
 * exactly the deadlock this helper lets a test avoid.
 *
 * Sometimes a test needs a select-waiter thread to REALLY get through Phase
 * 1's _ccol_mutex_lock and link step before it goes on, for example a test
 * that destroys cq to hit the has_sel_waiters misuse check in
 * __ccol_circular_queue_destroy every time. Such a test polls this function
 * in a bounded loop. The alternative is hoping a fixed sleep gives the OS
 * enough time to schedule a brand-new thread that far, but the return of
 * pthread_create() promises no such thing. If the guess loses the race,
 * destroy() goes on as if nobody watched the queue and frees cq under the
 * late waiter, right when that waiter finally runs. Under qemu-user
 * emulation thread-start scheduling latency is much higher and much more
 * variable, and a fixed 50ms margin isn't reliably enough there. */
bool _ccol_circq_test_has_sel_read_waiter_for_tests(ccol_circular_queue *cq) {
  _ccol_mutex_lock(cq->mutex);
  bool has_waiter = cq->sel_read_waiters_head != NULL;
  _ccol_mutex_unlock(cq->mutex);
  return has_waiter;
}

/* Test-only. How many waiter nodes are linked into cq's read-waiter list
 * right now. A test that needs a SPECIFIC number of ccol_select() callers,
 * not just "at least one", to reach their link step polls this. See its doc
 * comment in cthreadcomm.h. */
size_t _ccol_circq_test_sel_read_waiter_count_for_tests(
    ccol_circular_queue *cq) {
  _ccol_mutex_lock(cq->mutex);
  size_t n = 0;
  for (ccol_sel_waiter *w = cq->sel_read_waiters_head; w; w = w->next) ++n;
  _ccol_mutex_unlock(cq->mutex);
  return n;
}

/* Test-only. The counters that gate the send and receive paths' condition
 * variable signals. A test polls one of them to know that a background
 * thread really parked in the matching wait, and only then does the
 * operation that has to wake it. That keeps such a test deterministic and
 * non-vacuous, since a wait site that doesn't register itself never lets the
 * poll succeed. The read is under the mutex, which a parked waiter has
 * released. */
size_t _ccol_circq_waiting_readers_for_tests(ccol_circular_queue *cq) {
  _ccol_mutex_lock(cq->mutex);
  size_t n = cq->readers_waiting;
  _ccol_mutex_unlock(cq->mutex);
  return n;
}

size_t _ccol_circq_waiting_writers_for_tests(ccol_circular_queue *cq) {
  _ccol_mutex_lock(cq->mutex);
  size_t n = cq->writers_waiting;
  _ccol_mutex_unlock(cq->mutex);
  return n;
}
#endif

/* Writes msg into the circular array at write_index and moves the index on,
 * wrapping to 0 at max_size. It sets msg->data to NULL to hand the ownership
 * to the receiver, which is the zero-copy contract. The caller holds the
 * mutex. */
static void _sendto_cq(ccol_circular_queue *cq, c_message_t *msg) {
  cq->msg_array[cq->write_index].data = msg->data;
  cq->msg_array[cq->write_index++].size = (msg->data == NULL) ? 0 : msg->size;
  msg->data = NULL;
  if (cq->write_index == cq->max_size) {
    cq->write_index = 0;
  }
  ++cq->msg_count;

  if (cq->readers_waiting) _ccol_cond_var_signal(cq->read_cond);
  notify_one_sel_waiter(&cq->sel_read_waiters_head, &cq->sel_read_rotor);
}

/* Checks a send's arguments. Neither the queue nor the message may be NULL,
 * and the message's data and size must agree: a non-NULL data needs a
 * size above 0 (a live pointer can't carry an empty payload), and a NULL data
 * needs a size of 0 (a NULL pointer with a non-zero size makes no sense). */
static bool verify_circq_send_zc_params(ccol_circular_queue *cq,
                                        c_message_t *msg) {
  if (!cq || !msg || (msg->size == 0 && msg->data != NULL) ||
      (msg->data == NULL && msg->size != 0)) {
    return false;
  }

  return true;
}

/* Blocking send. It waits on write_cond until the queue has space and then
 * hands msg->data over to the queue. When sending is turned off it returns
 * ccol_not_permitted at once, and it checks that state both before and after
 * the wait to cope with a race against ccol_circq_disable_sending. */
ccol_retval_t ccol_circq_send_zc(ccol_circular_queue *cq, c_message_t *msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  _ccol_mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    _ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  while (cq->msg_count == cq->max_size && !cq->writing_disabled) {
    ++cq->writers_waiting;
    _ccol_cond_var_wait(cq->write_cond, cq->mutex);
    --cq->writers_waiting;
  }

  if (cq->writing_disabled) {
    _ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  _sendto_cq(cq, msg);

  _ccol_mutex_unlock(cq->mutex);

  return ccol_success;
}

/* Non-blocking send. It returns ccol_container_full right away when the
 * queue is full, and ccol_not_permitted when sending is turned off.
 */
ccol_retval_t ccol_circq_try_send_zc(ccol_circular_queue *cq,
                                     c_message_t *msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_full;

  _ccol_mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    _ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  if (cq->msg_count < cq->max_size) {
    _sendto_cq(cq, msg);
    result = ccol_success;
  }

  _ccol_mutex_unlock(cq->mutex);

  return result;
}

/* Forward declaration; the definition comes right before ccol_circq_recv_zc.
 * The racing-consumer simulation of ccol_circq_timed_send_zc needs it here,
 * and that simulation only exists under RUNNING_UNIT_TESTS. */
static void _recvfrom_cq(ccol_circular_queue *cq, c_message_t *target_buf);

#ifdef RUNNING_UNIT_TESTS
/* Test-only hooks. They make the next _ccol_cond_var_timedwait call in
 * ccol_circq_timed_send_zc's wait loop report EINVAL instead of a real wait
 * outcome, and then disarm themselves. The _racing_ready variant also frees
 * one slot's queue accounting with _recvfrom_cq, which is exactly what a real
 * concurrent consumer completing its receive would do, under the same
 * cq->mutex this call already holds. That simulates a consumer whose own
 * wakeup finished a moment before we see the unrelated forced error, with
 * the same reasoning as
 * _ccol_select_test_force_next_condvar_wait_error_racing_ready. Since we skip
 * the real wait and never unlock the mutex, no real concurrent thread could
 * race in here unaided, so this is the only way a test can get that
 * interleaving every time. When the freed slot carries message data, the
 * data goes back to the test through *racing_ready_msg_out for checking;
 * otherwise it would be thrown away here without a word. */
static _Atomic bool g_circq_send_force_condvar_wait_error = false;
static _Atomic bool g_circq_send_force_condvar_wait_error_also_race = false;
static c_message_t g_circq_send_race_freed_msg = {0};

void _ccol_circq_test_force_next_send_condvar_wait_error(void) {
  atomic_store(&g_circq_send_force_condvar_wait_error, true);
}

void _ccol_circq_test_force_next_send_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_circq_send_force_condvar_wait_error, true);
  atomic_store(&g_circq_send_force_condvar_wait_error_also_race, true);
}

c_message_t _ccol_circq_test_take_race_freed_msg(void) {
  c_message_t m = g_circq_send_race_freed_msg;
  g_circq_send_race_freed_msg = (c_message_t){0};
  return m;
}
#endif

/* Send with a timeout: it waits at most timeout_us microseconds for space,
 * and a timeout_us of 0 is exactly the try variant. The absolute deadline is
 * computed once before the wait loop, so spurious wakes can't stretch the
 * timeout, and once the deadline passes the function returns
 * ccol_timed_out. */
ccol_retval_t ccol_circq_timed_send_zc(ccol_circular_queue *cq,
                                       c_message_t *msg, uint64_t timeout_us) {
  if (timeout_us == 0) return ccol_circq_try_send_zc(cq, msg);
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  _ccol_mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    _ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  if (cq->msg_count == cq->max_size) {
    int retval;
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);

    while (cq->msg_count == cq->max_size && !cq->writing_disabled) {
      /* This wait is counted too, not only the untimed one, because the
       * other side signals only when this count isn't zero. Without it the
       * waiter never registers and sleeps until its own timeout while a
       * message is already queued for it. */
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
        retval = _ccol_cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
      }
#else
      retval = _ccol_cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
#endif
      --cq->writers_waiting;
      if (retval) {
        /* Look at the state again under the mutex before committing to
         * either outcome below, whatever _ccol_cond_var_timedwait returned.
         * It always takes cq->mutex again before returning, on success and
         * on failure, and a concurrent consumer's _recvfrom_cq needs
         * cq->mutex to free a slot and signal write_cond. So that consumer
         * can finish and hand the mutex back to this very call a moment
         * before we also see some unrelated spurious error that isn't
         * ETIMEDOUT. Without this second check we would throw away a real,
         * free slot and report ccol_unexpected_failure instead of sending
         * into it. The FAILURE branch of _sel_wait_condvar follows the same
         * reasoning. The check also covers a plain ETIMEDOUT, where a
         * consumer can free a slot, or writing_disabled can change, after
         * the kernel noticed the expiry but before this thread got the mutex
         * back. */
        if (cq->msg_count < cq->max_size || cq->writing_disabled) break;
        if (retval != ETIMEDOUT) {
          _ccol_store_wait_errno(retval);
          _ccol_mutex_unlock(cq->mutex);
          return ccol_unexpected_failure;
        }
        _ccol_mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
  }

  if (cq->writing_disabled) {
    _ccol_mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  _sendto_cq(cq, msg);

  _ccol_mutex_unlock(cq->mutex);

  return ccol_success;
}

/* The owner's send on cq, which ignores writing_disabled (see
 * internal/cqueueowner.h). It shares the timed send's wait for room, so a
 * receiver that frees a slot wakes it the same way. */
ccol_retval_t ccol_circq_owner_send_zc(ccol_circular_queue *cq,
                                       c_message_t *msg, uint64_t timeout_us) {
  if (!verify_circq_send_zc_params(cq, msg)) return ccol_invalid_args;
  _ccol_mutex_lock(cq->mutex);
  if (cq->msg_count == cq->max_size) {
    if (timeout_us == 0) {
      _ccol_mutex_unlock(cq->mutex);
      return ccol_container_full;
    }
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);
    while (cq->msg_count == cq->max_size) {
      ++cq->writers_waiting;
      int retval =
          _ccol_cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
      --cq->writers_waiting;
      if (retval && cq->msg_count == cq->max_size) {
        if (retval != ETIMEDOUT) _ccol_store_wait_errno(retval);
        _ccol_mutex_unlock(cq->mutex);
        return retval == ETIMEDOUT ? ccol_timed_out : ccol_unexpected_failure;
      }
    }
  }
  _sendto_cq(cq, msg);
  _ccol_mutex_unlock(cq->mutex);
  return ccol_success;
}

/* Reads one message from the circular array at read_index and moves the
 * index on, wrapping to 0 at max_size. It signals write_cond when a sender
 * is blocked on it. The caller holds the mutex. */
static void _recvfrom_cq(ccol_circular_queue *cq, c_message_t *target_buf) {
  target_buf->data = cq->msg_array[cq->read_index].data;
  target_buf->size = cq->msg_array[cq->read_index++].size;
  if (cq->read_index == cq->max_size) {
    cq->read_index = 0;
  }

  --cq->msg_count;

  if (cq->writers_waiting) _ccol_cond_var_signal(cq->write_cond);
  notify_one_sel_waiter(&cq->sel_write_waiters_head, &cq->sel_write_rotor);
}

/* Checks a receive's arguments: neither the queue nor the target buffer may
 * be NULL. */
static bool verify_recvfrom_cq_zc_params(ccol_circular_queue *cq,
                                         c_message_t *target_buf) {
  if (!cq || !target_buf) {
    return false;
  }

  return true;
}

/* Blocking receive. It waits on read_cond until the queue holds at least one
 * message and then hands its ownership over in target_buf. */
ccol_retval_t ccol_circq_recv_zc(ccol_circular_queue *cq,
                                 c_message_t *target_buf) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  _ccol_mutex_lock(cq->mutex);

  while (cq->msg_count == 0) {
    ++cq->readers_waiting;
    _ccol_cond_var_wait(cq->read_cond, cq->mutex);
    --cq->readers_waiting;
  }

  _recvfrom_cq(cq, target_buf);

  _ccol_mutex_unlock(cq->mutex);

  return ccol_success;
}

/* Non-blocking receive; it returns ccol_container_empty at once when the
 * queue is empty. */
ccol_retval_t ccol_circq_try_recv_zc(ccol_circular_queue *cq,
                                     c_message_t *target_buf) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_empty;

  _ccol_mutex_lock(cq->mutex);

  if (cq->msg_count > 0) {
    result = ccol_success;
    _recvfrom_cq(cq, target_buf);
  }

  _ccol_mutex_unlock(cq->mutex);

  return result;
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only hooks. They make the next _ccol_cond_var_timedwait call in
 * ccol_circq_timed_recv_zc's wait loop report EINVAL instead of a real wait
 * outcome, and then disarm themselves. The _racing_ready variant also puts a
 * sentinel message into the queue with _sendto_cq, exactly what a real
 * concurrent producer completing its send would do, under the same
 * cq->mutex this call already holds. That simulates a producer whose own
 * wakeup finished a moment before we see the unrelated forced error, with the
 * same reasoning as
 * _ccol_select_test_force_next_condvar_wait_error_racing_ready. Since the
 * real wait is skipped and the mutex never unlocked, no real concurrent
 * thread could race in here without help. */
static _Atomic bool g_circq_recv_force_condvar_wait_error = false;
static _Atomic bool g_circq_recv_force_condvar_wait_error_also_race = false;

void _ccol_circq_test_force_next_recv_condvar_wait_error(void) {
  atomic_store(&g_circq_recv_force_condvar_wait_error, true);
}

void _ccol_circq_test_force_next_recv_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_circq_recv_force_condvar_wait_error, true);
  atomic_store(&g_circq_recv_force_condvar_wait_error_also_race, true);
}
#endif

/* Receive with a timeout: it waits at most timeout_us microseconds for a
 * message, and a timeout_us of 0 is exactly the try variant. Like
 * ccol_circq_timed_send_zc it works from one absolute deadline, so a
 * spurious wake can't make the timeout drift.
 */
ccol_retval_t ccol_circq_timed_recv_zc(ccol_circular_queue *cq,
                                       c_message_t *target_buf,
                                       uint64_t timeout_us) {
  if (timeout_us == 0) return ccol_circq_try_recv_zc(cq, target_buf);
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  _ccol_mutex_lock(cq->mutex);

  if (cq->msg_count == 0) {
    int retval;
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);

    while (cq->msg_count == 0) {
      /* This wait is counted too, not only the untimed one, because the
       * other side signals only when this count isn't zero. Without it the
       * waiter never registers and sleeps until its own timeout while a
       * message is already queued for it. */
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
        retval = _ccol_cond_var_timedwait(cq->read_cond, cq->mutex, abs_time);
      }
#else
      retval = _ccol_cond_var_timedwait(cq->read_cond, cq->mutex, abs_time);
#endif
      --cq->readers_waiting;
      if (retval) {
        /* Look at the state again under the mutex before committing to
         * either outcome below, whatever _ccol_cond_var_timedwait returned.
         * It always takes cq->mutex again before returning, on success and
         * on failure, and a concurrent producer's _sendto_cq needs cq->mutex
         * to add a message and signal read_cond. So that producer can finish
         * and hand the mutex back to this very call a moment before we also
         * see some unrelated spurious error that isn't ETIMEDOUT. Without
         * this second check we would throw away a message that really
         * arrived and report ccol_unexpected_failure instead of receiving
         * it. The FAILURE branch of _sel_wait_condvar follows the same
         * reasoning. The check also covers a plain ETIMEDOUT, where a
         * producer can add a message after the kernel noticed the expiry but
         * before this thread got the mutex back. */
        if (cq->msg_count > 0) break;
        if (retval != ETIMEDOUT) {
          _ccol_store_wait_errno(retval);
          _ccol_mutex_unlock(cq->mutex);
          return ccol_unexpected_failure;
        }
        _ccol_mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
  }

  _recvfrom_cq(cq, target_buf);

  _ccol_mutex_unlock(cq->mutex);

  return ccol_success;
}

/* Sets writing_disabled and broadcasts on write_cond, waking every thread
 * blocked in ccol_circq_send_zc so each of them sees the new state and
 * returns ccol_not_permitted. */
ccol_retval_t ccol_circq_disable_sending(ccol_circular_queue *cq) {
  if (cq) {
    _ccol_mutex_lock(cq->mutex);
    cq->writing_disabled = true;
    _ccol_cond_var_broadcast(cq->write_cond);
    notify_all_sel_waiters(cq->sel_write_waiters_head);
    _ccol_mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

/* Clears writing_disabled and broadcasts on write_cond, waking every thread
 * that blocked while sending was turned off. */
ccol_retval_t ccol_circq_enable_sending(ccol_circular_queue *cq) {
  if (cq) {
    _ccol_mutex_lock(cq->mutex);
    cq->writing_disabled = false;
    _ccol_cond_var_broadcast(cq->write_cond);
    notify_all_sel_waiters(cq->sel_write_waiters_head);
    _ccol_mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

/* Returns how many messages the queue holds right now, taking the mutex for
 * a consistent snapshot, or ccol_invalid_size when cq is NULL. */
size_t ccol_circq_msg_count(ccol_circular_queue *cq) {
  size_t result = ccol_invalid_size;

  if (cq) {
    _ccol_mutex_lock(cq->mutex);
    result = cq->msg_count;
    _ccol_mutex_unlock(cq->mutex);
  }

  return result;
}

/* From here on: the dynamic queue. */
typedef struct dllist_node {
  struct dllist_node *prev;
  c_message_t msg;
  struct dllist_node *next;
} dllist_node;

struct ccol_dynamic_queue {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t read_cond;

  /* See the same field on ccol_circular_queue for why the signal is gated
   * instead of sent on every operation. This queue is unbounded, so it has
   * readers to wake but never a blocked writer. */
  size_t readers_waiting;

  size_t msg_count;

  dllist_node *head;
  dllist_node *tail;

  ccol_memmgmt_procs_t *m_procs;

  bool writing_disabled;

  ccol_sel_waiter *sel_read_waiters_head;
  ccol_sel_waiter *sel_write_waiters_head;

  /* Round-robin cursors for notify_one_sel_waiter (see its doc comment).
   * NULL means "start a fresh cycle at the matching head". */
  ccol_sel_waiter *sel_read_rotor;
  ccol_sel_waiter *sel_write_rotor;
  /* Registrations of shut-down ccol_event_loop instances that still name this
   * queue although their waiter nodes have left the lists above (see
   * _ccol_event_loop_detach_queue_regs). A destroy refuses the queue while
   * this isn't zero, just as while a waiter is linked. Guarded by mutex. */
  size_t detached_loop_regs;

  /* See the same field on ccol_circular_queue. */
  _Atomic size_t dispatch_refs;

#if CCOL_FORK_SAFETY_REQUIRED
  /* See the same field on ccol_circular_queue. */
  size_t registry_idx;
#endif
};

/* Allocates a new dllist_node, copies the message's metadata into it and
 * sets msg->data to NULL to hand over the ownership, then appends the node at
 * the tail of the queue's doubly-linked list. The caller holds the mutex. */
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
    _ccol_assert(!dq->tail);
#endif
    new_elem->prev = NULL;
    dq->head = new_elem;
    dq->tail = new_elem;
  } else {
#ifdef RUNNING_UNIT_TESTS
    _ccol_assert(dq->tail && !dq->tail->next);
#endif
    new_elem->prev = dq->tail;
    dq->tail->next = new_elem;
    dq->tail = new_elem;
  }

  return ccol_success;
}

/* Takes the head node's message and gives it to the caller. When the list
 * goes empty it sets both head and tail to NULL so the invariant holds, and
 * it frees the node struct once the message is copied out. The caller holds
 * the mutex. */
static ccol_retval_t remove_msg_from_dq_head(ccol_dynamic_queue *dq,
                                             c_message_t *target_buf) {
  if (!dq->head) {
#ifdef RUNNING_UNIT_TESTS
    _ccol_assert(!dq->tail);
#endif
    return ccol_container_empty;
  }

#ifdef RUNNING_UNIT_TESTS
  _ccol_assert(!dq->head->prev);
  _ccol_assert(dq->tail && !dq->tail->next);
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

/* Frees every dllist_node struct in the dynamic queue but not the data
 * pointers of the messages, which a consumer should already have taken; the
 * destroy function asserts on a non-zero msg_count to catch that leak. */
static void destroy_dq_dllist(ccol_dynamic_queue *dq) {
  dllist_node *node_to_be_freed = NULL;
  while (dq->head) {
    node_to_be_freed = dq->head;
    dq->head = dq->head->next;
    _ccol_mem_free(dq->m_procs, node_to_be_freed);
  }
  dq->tail = NULL;
}

/* Allocates and initialises an unbounded dynamic queue backed by a
 * doubly-linked list. A send here never blocks, unlike on a
 * ccol_circular_queue, because the list just grows with every message, so
 * this queue needs a read_cond but no write_cond. */
ccol_dynamic_queue *ccol_dynamic_queue_create_mp(
    ccol_memmgmt_procs_t *mmgmt_procs, char **err_str) {
  if (!_ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  ccol_dynamic_queue *dq = (ccol_dynamic_queue *)_ccol_mem_alloc(
      mmgmt_procs, sizeof(ccol_dynamic_queue));
  if (!dq) {
    if (err_str) {
      *err_str =
          _CCOL_ERR_STR("Failed to allocate memory for ccol_dynamic_queue");
    }
    return NULL;
  }

  if (!_ccol_populate_mem_mgmt_procs(dq, mmgmt_procs, err_str)) {
    _ccol_mem_free(mmgmt_procs, dq);
    return NULL;
  }

  if (_ccol_mutex_init(dq->mutex) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("Failed to initialize dq mutex");
    _ccol_mem_free(mmgmt_procs, dq->m_procs);
    _ccol_mem_free(mmgmt_procs, dq);
    return NULL;
  }
  if (_init_monotonic_cond_var(&dq->read_cond) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("Failed to initialize dq read_cond");
    _ccol_mutex_destroy(dq->mutex);
    _ccol_mem_free(mmgmt_procs, dq->m_procs);
    _ccol_mem_free(mmgmt_procs, dq);
    return NULL;
  }
  dq->msg_count = 0;
  dq->head = NULL;
  dq->tail = NULL;
  /* Set explicitly for the same reason as ccol_circular_queue's counters;
   * see the note there. */
  dq->readers_waiting = 0;
  dq->writing_disabled = false;
  dq->sel_read_waiters_head = NULL;
  dq->sel_write_waiters_head = NULL;
  dq->sel_read_rotor = NULL;
  dq->sel_write_rotor = NULL;
  dq->detached_loop_regs = 0;
  atomic_init(&dq->dispatch_refs, (size_t)0);

#if CCOL_FORK_SAFETY_REQUIRED
  /* Registering is the LAST step, once dq is fully built, for the reason
   * given at the same call in ccol_circular_queue_create_mp. A
   * ccol_dynamic_queue has no msg_array to unwind, since it starts out as an
   * empty linked list. */
  if (!_queue_mutex_registry_add(&dq->mutex, &dq->registry_idx)) {
    if (err_str) {
      *err_str = _CCOL_ERR_STR(
          "Failed to register ccol_dynamic_queue's mutex for fork safety");
    }
    _ccol_mutex_destroy(dq->mutex);
    _ccol_cond_var_destroy(dq->read_cond);
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

/* Destroys the dynamic queue. Like __ccol_circular_queue_destroy it asserts
 * when a message is left (so a data pointer nobody cleaned up shows up as a
 * bug) and when a ccol_select() or ccol_event_loop waiter is still linked
 * into one of the waiter lists. The comment of __ccol_circular_queue_destroy
 * explains why the second case is a real use-after-free hazard and not just
 * a leak: the linked node would then hold a dangling &dq->mutex. */
void __ccol_dynamic_queue_destroy(ccol_dynamic_queue *dq) {
  if (dq) {
    /* Read under the mutex; the same check in __ccol_circular_queue_destroy
     * explains why an unlocked peek isn't good enough. */
    _ccol_mutex_lock(dq->mutex);
    bool has_sel_waiters = (dq->sel_read_waiters_head != NULL) ||
                           (dq->sel_write_waiters_head != NULL) ||
                           dq->detached_loop_regs != 0;
    _ccol_mutex_unlock(dq->mutex);
    if (has_sel_waiters) {
      _ccol_assert(false);
    }

    /* Same wait as in __ccol_circular_queue_destroy, with the same count
     * check after it. */
    _queue_wait_for_dispatch_refs(&dq->dispatch_refs, dq);

    if (ccol_dynmq_msg_count(dq) > 0) {
      _ccol_assert(false);
    }

#if CCOL_FORK_SAFETY_REQUIRED
    /* Unregister the mutex before destroying it, for the reason given at the
     * same call in __ccol_circular_queue_destroy. */
    _queue_mutex_registry_remove(&dq->mutex, &dq->registry_idx);
#endif

    _ccol_mutex_destroy(dq->mutex);
    _ccol_cond_var_destroy(dq->read_cond);
    destroy_dq_dllist(dq);

    if (dq->m_procs) {
      ccol_free_t free_func = dq->m_procs->free;
      free_func(dq->m_procs);
      free_func(dq);
    } else {
      _ccol_default_mem_free(dq);
    }
  }
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. The counter that gates the dynamic queue's read_cond signal on
 * the send path. _ccol_circq_waiting_readers_for_tests explains why a test
 * polls it instead of guessing that a background thread has parked. */
size_t _ccol_dynmq_waiting_readers_for_tests(ccol_dynamic_queue *dq) {
  _ccol_mutex_lock(dq->mutex);
  size_t n = dq->readers_waiting;
  _ccol_mutex_unlock(dq->mutex);
  return n;
}
#endif

/* Appends msg at the tail of the dynamic queue and signals read_cond when a
 * receiver is blocked on it. The caller holds the mutex. If the allocation
 * fails it returns ccol_not_enough_memory and leaves msg->data alone. */
static ccol_retval_t _sendto_dq(ccol_dynamic_queue *dq, c_message_t *msg) {
  ccol_retval_t retval = append_msg_to_dq_tail(dq, msg);

  if (retval == ccol_success) {
    ++dq->msg_count;
    if (dq->readers_waiting) _ccol_cond_var_signal(dq->read_cond);
    notify_one_sel_waiter(&dq->sel_read_waiters_head, &dq->sel_read_rotor);
  }

  return retval;
}

/* Checks a send's arguments the same way verify_circq_send_zc_params does,
 * but for a ccol_dynamic_queue. */
static bool verify_dynmq_send_zc_params(ccol_dynamic_queue *dq,
                                        c_message_t *msg) {
  if (!dq || !msg || (msg->size == 0 && msg->data != NULL) ||
      (msg->data == NULL && msg->size != 0)) {
    return false;
  }

  return true;
}

/* Non-blocking send to the dynamic queue, which is unbounded and so never
 * waits for space. It returns ccol_not_permitted when sending is turned off
 * and ccol_container_full once msg_count reaches ccol_max_elem_count. */
ccol_retval_t ccol_dynmq_send_zc(ccol_dynamic_queue *dq, c_message_t *msg) {
  if (!verify_dynmq_send_zc_params(dq, msg)) {
    return ccol_invalid_args;
  }

  _ccol_mutex_lock(dq->mutex);

  if (dq->writing_disabled) {
    _ccol_mutex_unlock(dq->mutex);
    return ccol_not_permitted;
  }

  if (dq->msg_count == ccol_max_elem_count) {
    _ccol_mutex_unlock(dq->mutex);
    return ccol_container_full;
  }

  ccol_retval_t result = _sendto_dq(dq, msg);

  _ccol_mutex_unlock(dq->mutex);

  return result;
}

/* The owner's send on dq, which ignores writing_disabled (see
 * internal/cqueueowner.h). */
ccol_retval_t ccol_dynmq_owner_send_zc(ccol_dynamic_queue *dq,
                                       c_message_t *msg) {
  if (!verify_dynmq_send_zc_params(dq, msg)) return ccol_invalid_args;
  _ccol_mutex_lock(dq->mutex);
  ccol_retval_t result = dq->msg_count == ccol_max_elem_count
                             ? ccol_container_full
                             : _sendto_dq(dq, msg);
  _ccol_mutex_unlock(dq->mutex);
  return result;
}

/* Takes the head message off the dynamic queue and decrements msg_count. The
 * caller holds the mutex. */
static ccol_retval_t _recvfrom_dq(ccol_dynamic_queue *dq,
                                  c_message_t *target_buf) {
  ccol_retval_t retval = remove_msg_from_dq_head(dq, target_buf);

  if (retval == ccol_success) {
    --dq->msg_count;
    notify_one_sel_waiter(&dq->sel_write_waiters_head, &dq->sel_write_rotor);
  }

  return retval;
}

/* Checks the arguments of a receive on a dynamic queue. */
static bool verify_recvfrom_dq_zc_params(ccol_dynamic_queue *dq,
                                         c_message_t *target_buf) {
  if (!dq || !target_buf) {
    return false;
  }

  return true;
}

/* Blocking receive from the dynamic queue. It waits on read_cond until the
 * queue holds at least one message and then pops it off the head. */
ccol_retval_t ccol_dynmq_recv_zc(ccol_dynamic_queue *dq,
                                 c_message_t *target_buf) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  _ccol_mutex_lock(dq->mutex);

  while (dq->msg_count == 0) {
    ++dq->readers_waiting;
    _ccol_cond_var_wait(dq->read_cond, dq->mutex);
    --dq->readers_waiting;
  }

  ccol_retval_t result = _recvfrom_dq(dq, target_buf);

  _ccol_mutex_unlock(dq->mutex);

  return result;
}

/* Non-blocking receive; it returns ccol_container_empty immediately when the
 * dynamic queue is empty. */
ccol_retval_t ccol_dynmq_try_recv_zc(ccol_dynamic_queue *dq,
                                     c_message_t *target_buf) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_empty;

  _ccol_mutex_lock(dq->mutex);

  if (dq->msg_count > 0) {
    result = _recvfrom_dq(dq, target_buf);
  }

  _ccol_mutex_unlock(dq->mutex);

  return result;
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only hooks. They make the next _ccol_cond_var_timedwait call in
 * ccol_dynmq_timed_recv_zc's wait loop report EINVAL instead of a real wait
 * outcome, and then disarm themselves. The _racing_ready variant also puts a
 * sentinel message into the queue with _sendto_dq, exactly what a real
 * concurrent producer completing its send would do, under the same
 * dq->mutex this call already holds. That simulates a producer whose own
 * wakeup finished a moment before we see the unrelated forced error, with the
 * same reasoning as
 * _ccol_select_test_force_next_condvar_wait_error_racing_ready. Since the
 * real wait is skipped and the mutex never unlocked, no real concurrent
 * thread could race in here on its own. */
static _Atomic bool g_dynmq_recv_force_condvar_wait_error = false;
static _Atomic bool g_dynmq_recv_force_condvar_wait_error_also_race = false;

void _ccol_dynmq_test_force_next_recv_condvar_wait_error(void) {
  atomic_store(&g_dynmq_recv_force_condvar_wait_error, true);
}

void _ccol_dynmq_test_force_next_recv_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_dynmq_recv_force_condvar_wait_error, true);
  atomic_store(&g_dynmq_recv_force_condvar_wait_error_also_race, true);
}
#endif

/* Receive from the dynamic queue with a timeout, where a timeout_us of 0 is
 * exactly the try variant. It works from one absolute deadline like the
 * circular queue's timed variants. */
ccol_retval_t ccol_dynmq_timed_recv_zc(ccol_dynamic_queue *dq,
                                       c_message_t *target_buf,
                                       uint64_t timeout_us) {
  if (timeout_us == 0) return ccol_dynmq_try_recv_zc(dq, target_buf);
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  _ccol_mutex_lock(dq->mutex);

  if (dq->msg_count == 0) {
    int retval;
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);

    while (dq->msg_count == 0) {
      /* This wait is counted too, not only the untimed one, because the
       * other side signals only when this count isn't zero. Without it the
       * waiter never registers and sleeps until its own timeout while a
       * message is already queued for it. */
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
        retval = _ccol_cond_var_timedwait(dq->read_cond, dq->mutex, abs_time);
      }
#else
      retval = _ccol_cond_var_timedwait(dq->read_cond, dq->mutex, abs_time);
#endif
      --dq->readers_waiting;
      if (retval) {
        /* Look at the state again under the mutex before committing to
         * either outcome below, whatever _ccol_cond_var_timedwait returned.
         * It always takes dq->mutex again before returning, on success and
         * on failure, and a concurrent producer's _sendto_dq needs dq->mutex
         * to add a message and signal read_cond. So that producer can finish
         * and hand the mutex back to this very call a moment before we also
         * see some unrelated spurious error that isn't ETIMEDOUT. Without
         * this second check we would throw away a message that really
         * arrived and report ccol_unexpected_failure instead of receiving
         * it. The FAILURE branch of _sel_wait_condvar follows the same
         * reasoning. The check also covers a plain ETIMEDOUT, where a
         * producer can add a message after the kernel noticed the expiry but
         * before this thread got the mutex back. */
        if (dq->msg_count > 0) break;
        if (retval != ETIMEDOUT) {
          _ccol_store_wait_errno(retval);
          _ccol_mutex_unlock(dq->mutex);
          return ccol_unexpected_failure;
        }
        _ccol_mutex_unlock(dq->mutex);
        return ccol_timed_out;
      }
    }
  }

  ccol_retval_t result = _recvfrom_dq(dq, target_buf);

  _ccol_mutex_unlock(dq->mutex);

  return result;
}

/* Sets writing_disabled on the dynamic queue. */
ccol_retval_t ccol_dynmq_disable_sending(ccol_dynamic_queue *dq) {
  if (dq) {
    _ccol_mutex_lock(dq->mutex);
    dq->writing_disabled = true;
    notify_all_sel_waiters(dq->sel_write_waiters_head);
    _ccol_mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

/* Clears writing_disabled on the dynamic queue. */
ccol_retval_t ccol_dynmq_enable_sending(ccol_dynamic_queue *dq) {
  if (dq) {
    _ccol_mutex_lock(dq->mutex);
    dq->writing_disabled = false;
    notify_all_sel_waiters(dq->sel_write_waiters_head);
    _ccol_mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

/* Returns how many messages the dynamic queue holds, or ccol_invalid_size
 * when dq is NULL. */
size_t ccol_dynmq_msg_count(ccol_dynamic_queue *dq) {
  size_t result = ccol_invalid_size;

  if (dq) {
    _ccol_mutex_lock(dq->mutex);
    result = dq->msg_count;
    _ccol_mutex_unlock(dq->mutex);
  }

  return result;
}

/* From here on: the channel. */
struct ccol_channel {
  _ccol_thread_id_t owner_tid;
  ccol_circular_queue *owner_to_workers_cq;
  ccol_circular_queue *workers_to_owner_cq;
  ccol_memmgmt_procs_t *m_procs;
};

/* Creates a ccol_channel that carries messages both ways over two circular
 * queues, one from the owner to the workers and one from the workers back to
 * the owner. The creating thread's ID is recorded as owner_tid, which is how
 * ccol_chan_send_zc and ccol_chan_recv_zc pick the right queue by
 * themselves. */
ccol_channel *ccol_channel_create_mp(size_t max_size,
                                     ccol_memmgmt_procs_t *mmgmt_procs,
                                     char **err_str) {
  if (!_ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  ccol_channel *ch =
      (ccol_channel *)_ccol_mem_alloc(mmgmt_procs, sizeof(ccol_channel));
  if (!ch) {
    if (err_str) {
      *err_str = _CCOL_ERR_STR("Failed to allocate memory for ccol_channel");
    }
    return NULL;
  }

  if (!_ccol_populate_mem_mgmt_procs(ch, mmgmt_procs, err_str)) {
    _ccol_mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->owner_to_workers_cq =
      ccol_circular_queue_create_mp(max_size, mmgmt_procs, err_str);
  if (!ch->owner_to_workers_cq) {
    _ccol_mem_free(mmgmt_procs, ch->m_procs);
    _ccol_mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->workers_to_owner_cq =
      ccol_circular_queue_create_mp(max_size, mmgmt_procs, err_str);
  if (!ch->workers_to_owner_cq) {
    ccol_circular_queue_destroy(ch->owner_to_workers_cq);
    _ccol_mem_free(mmgmt_procs, ch->m_procs);
    _ccol_mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->owner_tid = _ccol_get_thread_id();

  if (err_str) {
    *err_str = NULL;
  }

  return ch;
}

/* Destroys both of the channel's circular queues and then frees the
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
      _ccol_default_mem_free(ch);
    }
  }
}

/* Blocking send on the ccol_channel. Called from the owner thread it goes to
 * owner_to_workers_cq, and from any other thread to workers_to_owner_cq. The
 * thread identity check is the whole routing mechanism and costs next to
 * nothing, so the caller doesn't need a direction parameter. */
ccol_retval_t ccol_chan_send_zc(ccol_channel *ch, c_message_t *msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (_ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_send_zc(ch->owner_to_workers_cq, msg);
  }

  return ccol_circq_send_zc(ch->workers_to_owner_cq, msg);
}

/* Non-blocking send on the ccol_channel, in the direction of the caller. */
ccol_retval_t ccol_chan_try_send_zc(ccol_channel *ch, c_message_t *msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (_ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_try_send_zc(ch->owner_to_workers_cq, msg);
  }

  return ccol_circq_try_send_zc(ch->workers_to_owner_cq, msg);
}

/* Send on the ccol_channel with a timeout; it picks the direction by
 * itself. */
ccol_retval_t ccol_chan_timed_send_zc(ccol_channel *ch, c_message_t *msg,
                                      uint64_t timeout_us) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (_ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_timed_send_zc(ch->owner_to_workers_cq, msg, timeout_us);
  }

  return ccol_circq_timed_send_zc(ch->workers_to_owner_cq, msg, timeout_us);
}

/* Blocking receive on the ccol_channel, in the direction of the caller:
 * the owner thread receives from workers_to_owner_cq and a worker thread from
 * owner_to_workers_cq. */
ccol_retval_t ccol_chan_recv_zc(ccol_channel *ch, c_message_t *target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (_ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_recv_zc(ch->workers_to_owner_cq, target_buf);
  }

  return ccol_circq_recv_zc(ch->owner_to_workers_cq, target_buf);
}

/* Non-blocking receive on the ccol_channel; it picks the direction by
 * itself. */
ccol_retval_t ccol_chan_try_recv_zc(ccol_channel *ch, c_message_t *target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (_ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_try_recv_zc(ch->workers_to_owner_cq, target_buf);
  }

  return ccol_circq_try_recv_zc(ch->owner_to_workers_cq, target_buf);
}

/* Receive on the ccol_channel with a timeout; it picks the direction by
 * itself. */
ccol_retval_t ccol_chan_timed_recv_zc(ccol_channel *ch, c_message_t *target_buf,
                                      uint64_t timeout_us) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (_ccol_get_thread_id() == ch->owner_tid) {
    return ccol_circq_timed_recv_zc(ch->workers_to_owner_cq, target_buf,
                                    timeout_us);
  }

  return ccol_circq_timed_recv_zc(ch->owner_to_workers_cq, target_buf,
                                  timeout_us);
}

/* Turns off sending on the given direction, ccol_owner_to_workers or
 * ccol_workers_to_owner. Here the direction must be explicit, since the
 * caller may want to turn off only one side of the ccol_channel.
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

/* Turns sending back on for the given direction of the ccol_channel. */
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

/* Returns the message count of the circular queue behind the given
 * direction, or ccol_invalid_size for a direction it doesn't know. */
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

/* From here on: ccol_select. */

/* Splices node out of a doubly-linked waiter list. The caller already holds
 * the owning queue's mutex, and every caller pairs the splice with a forward
 * of the wake in the same critical section, so a listener leaving a ready
 * queue hands on any wake it may hold. head and rotor must be the addresses
 * of the right pair in the owning queue: sel_read_waiters_head with
 * sel_read_rotor, or sel_write_waiters_head with sel_write_rotor.
 *
 * rotor must be the round-robin cursor of the SAME list (see the doc
 * comment of notify_one_sel_waiter). If node is the rotor's current target,
 * the rotor moves to node's .next BEFORE node is spliced out, becoming NULL
 * when there is no .next so the cycle starts at *head again. That step is
 * required: right after the unlink node can be freed (a ccol_event_loop
 * registration) or go out of scope (a ccol_select() waiter, whose heap node
 * array is freed when the call returns), and a rotor still pointing at it
 * would be a use-after-free, or a read of invalid stack memory, the next
 * time notify_one_sel_waiter dereferences it. */
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

/* Both of these are defined next to the ccol_event_loop code further below,
 * where their other callers are, and forward declared here because
 * deregister_sel_waiter needs them. The first resolves a queue selectable to
 * its owning queue's mutex and the head and rotor pair its direction uses,
 * the second says whether that direction is ready right now, and in both
 * cases the caller holds the queue's mutex. We call them here instead of
 * restating either, so a leaving waiter uses the same single idea of
 * "ready" that every producer, scan and cascade step in this file uses. */
static void _queue_sel_locate(ccol_selectable *sel, _ccol_mutex_t **out_mtx,
                              ccol_sel_waiter ***out_head,
                              ccol_sel_waiter ***out_rotor);
static bool _queue_sel_is_ready(ccol_selectable *sel);

/* Removes the waiter node at index i from its queue's waiter list, taking
 * and releasing the queue's mutex itself. It sets nodes[i].sel_mtx to NULL
 * to mark the slot deregistered, so a later deregister_all_sel_waiters call
 * safely skips it.
 *
 * If the selectable is ready while this node leaves the list, the wake is
 * forwarded in the same critical section as the splice. A producer wakes
 * exactly ONE select waiter (through notify_one_sel_waiter) per message it
 * sends and per slot it frees, and the eventfd drain below throws away a
 * notification that landed on this node after its owner stopped waiting.
 *
 * Without the forward, a wake sent to a waiter that then gives up for some
 * unrelated reason (its deadline passed, another selectable of the same
 * array won, one of its fds fired, or an error) is used up and never
 * replaced. A sibling waiter then stays parked while a message sits in the
 * queue, which breaks the guarantee in ccol_event_loop_add's documentation
 * that no live listener is skipped indefinitely, and it isn't the "receive
 * on an empty queue blocks" contract either, because the queue isn't empty.
 *
 * The forward runs after the splice, so it can't land back on this
 * departing node: _sel_unlink_waiter_locked has already moved the rotor off
 * it, so the rotor names a linked node. */
static void deregister_sel_waiter(size_t i, ccol_sel_waiter *nodes,
                                  ccol_selectable *selectables) {
  /* An fd selectable has no waiter list, so there's nothing to unlink. */
  if (selectables[i].type == ccol_selectable_fd) return;

  _ccol_mutex_t *q_mtx;
  ccol_sel_waiter **head;
  ccol_sel_waiter **rotor;
  _queue_sel_locate(&selectables[i], &q_mtx, &head, &rotor);

  _ccol_mutex_lock(*q_mtx);
  _sel_unlink_waiter_locked(&nodes[i], head, rotor);
  if (_queue_sel_is_ready(&selectables[i])) notify_one_sel_waiter(head, rotor);
  _ccol_mutex_unlock(*q_mtx);

  /* Drain the eventfd instead of closing it, since the next iteration reuses
   * it. EFD_NONBLOCK is set, so this read returns EAGAIN when the counter is
   * already 0, which happens when the producer wrote while this thread was
   * busy with another event. The queue mutex serialises the drain and the
   * producer's write, so the two can't race. */
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

/* Works out which of a ccol_channel's two internal queues the calling thread
 * should watch, from the thread's identity and the direction it asks for.
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
  bool is_owner = (_ccol_get_thread_id() == ch->owner_tid);
  ccol_circular_queue *cq;
  if (dir == ccol_select_read) {
    cq = is_owner ? ch->workers_to_owner_cq : ch->owner_to_workers_cq;
  } else {
    cq = is_owner ? ch->owner_to_workers_cq : ch->workers_to_owner_cq;
  }
  return (ccol_selectable){.type = ccol_selectable_circq, .dir = dir, .cq = cq};
}

/* Allocates an eventfd for nodes[i] if it has none yet and registers it with
 * epfd under EPOLLIN. The eventfd is made once per call and reused on every
 * loop iteration, so a later call with nodes[i].efd >= 0 does nothing.
 * Returns true on success and false on any system error from eventfd or
 * epoll_ctl. The node must not be linked into its queue's waiter list here,
 * because producers read efd. */
static bool _sel_ensure_efd(size_t i, ccol_sel_waiter *nodes, int epfd) {
  if (nodes[i].efd >= 0) return true;
  nodes[i].efd = ccol_wakefd_create();
  if (nodes[i].efd < 0) return false;
  ccol_poll_event ev = {.data.u64 = (uint64_t)i, .events = CCOL_POLL_IN};
  if (ccol_poll_ctl(epfd, CCOL_POLL_CTL_ADD, nodes[i].efd, &ev, NULL) < 0) {
    close(nodes[i].efd);
    nodes[i].efd = -1;
    return false;
  }
  return true;
}

/* Fills in nodes[i] and prepends it to *head. The caller holds the owning
 * queue's mutex and unlocks it after this returns. */
static void _sel_link_waiter(size_t i, ccol_sel_waiter *nodes,
                             _ccol_mutex_t *sel_mtx, _ccol_cond_var_t *sel_cond,
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

/* Returns ccol_success when every argument is valid and ccol_invalid_args
 * otherwise.
 */
static ccol_retval_t _sel_validate_args(const size_t *ready_index, size_t n,
                                        const ccol_selectable *selectables) {
  if (!ready_index || n == 0 || !selectables) return ccol_invalid_args;
  /* ccol_select_timed() keeps one waiter node per selectable in an array it
   * gets from a single plain malloc() of n * sizeof(ccol_sel_waiter) bytes, a
   * few lines after it calls this function. That malloc() doesn't check for
   * overflow, so this check guards the multiplication itself before any
   * allocation happens. verify_circular_queue_create_inputs and
   * ccol_event_loop_create_mp use the same SIZE_MAX / element_size idiom for
   * the same kind of allocation in this file. It covers _sel_fd_state_init's
   * block too, since n_fd <= n and a pair, a pollfd and a flag together are
   * no larger than a ccol_sel_waiter, although that function has its own
   * guard as well instead of relying on this one alone. */
  if (n > SIZE_MAX / sizeof(ccol_sel_waiter)) return ccol_invalid_args;
  /* _sel_phase1_scan_register narrows a matched selectable's index, a size_t
   * from 0 to n-1, into a plain `int`, its `found` local, which also carries
   * the sentinels -1 ("nothing found yet") and -2 ("system error").
   * ccol_select_timed widens it back with `(size_t)found`. An n big enough
   * to give a match index above INT_MAX would narrow to a negative or wrapped
   * value there and corrupt *ready_index. The SIZE_MAX /
   * sizeof(ccol_sel_waiter) guard above doesn't catch this, since that bound
   * is many orders of magnitude above INT_MAX on any 64-bit platform.
   * ccol_event_loop_create_mp rejects an n > INT_MAX for max_events_per_wait
   * the same way, against the same kind of size_t to int narrowing. */
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
 * value except UINT64_MAX it fills *deadline and returns true (a value too
 * large for the clock saturates). For UINT64_MAX it returns false, meaning a
 * wait with no end. */
static bool _sel_compute_deadline(uint64_t timeout_us,
                                  struct timespec *deadline) {
  if (timeout_us == UINT64_MAX) return false;
  ccol_deadline_after_us(timeout_us, deadline);
  return true;
}

/* The base epoll interest flags of one direction on an fd selectable.
 * _sel_setup_epoll uses them for the registration and _sel_wait_epoll uses
 * them to map a combined fd event back to the member selectable it
 * satisfies, and since both call this function they can never disagree. */
static uint32_t _sel_fd_base_events(ccol_select_dir dir) {
  return (dir == ccol_select_read)
             ? (uint32_t)(CCOL_POLL_IN | CCOL_POLL_RDHUP | CCOL_POLL_ERR |
                          CCOL_POLL_HUP)
             : (uint32_t)(CCOL_POLL_OUT | CCOL_POLL_ERR | CCOL_POLL_HUP);
}

/* Tag bit in the ev.data.u64 of an fd group's epoll registration, with the
 * real fd value in the other 63 bits. It can't collide with a queue
 * selectable's ev.data.u64, which is a plain index into selectables[] and
 * always below n (see _sel_ensure_efd); a real n never gets near 2^63,
 * because the caller would need an actual selectables[] array that big. The
 * tag is how _sel_mark_fd_events tells a fired fd from a fired queue eventfd
 * by reading ev.data.u64 alone. The other bits hold the position of the fd
 * group's first member in _sel_fd_state's sorted pairs, so an event leads to
 * its group without a search; that position is below INT_MAX, which keeps
 * bit 62 clear, the bit that cpoll.h's kqueue translation reserves. */
#define _SEL_FD_TAG ((uint64_t)1 << 63)

/* One fd selectable with its own index into selectables[]. An array of these
 * is sorted by fd so that all the selectables sharing one real fd end up in
 * one contiguous run (they can mix directions and can be exact duplicates).
 * See _sel_setup_epoll. */
typedef struct {
  int fd;
  size_t idx;
} _sel_fd_pair;

/* What a ccol_select_timed() call with fd selectables keeps for the whole
 * call. pairs holds every fd selectable with its index and pfds one pollfd
 * for each of them in the same order, since Phase 1 asks poll(2) about them
 * once per pass and that needs no kernel state. Only a call that has to
 * block builds the epoll set (_sel_setup_epoll), which sorts pairs, and pfds
 * along with it, by fd and then by index, so one fd group's members form a
 * contiguous run in the caller's array order. evs is the event buffer for
 * every wait on that set and has room for every filter the set can report
 * at once (one registration per fd group and per queue eventfd, and two
 * filters per registration on kqueue), so a single wait sees every ready fd.
 * fd_ready has one flag per selectable, set by the last poll or wait for
 * each fd selectable it reported ready in its own direction. pairs, pfds and
 * fd_ready share one allocation. */
typedef struct {
  _sel_fd_pair *pairs;
  struct pollfd *pfds;
  size_t n_pairs;
  ccol_poll_event *evs;
  int ev_cap;
  bool *fd_ready;
  bool has_queues;
} _sel_fd_state;

static void _sel_fd_state_free(_sel_fd_state *fs) {
  free(fs->pairs);
  free(fs->evs);
}

/* The calling thread's rotation state for ccol_select_timed()'s scan of the
 * selectables. Each call with at least two selectables adds the 32-bit
 * golden ratio to it (a Weyl sequence) and scales it to a start index in
 * [0, n). Consecutive starts spread evenly over [0, n) for every n, even when
 * one thread alternates between several selectable sets, so each selectable
 * that stays ready wins its share of calls and a ready selectable is at most
 * a bounded number of calls behind any other. Being per thread, the rotation
 * writes no shared cache line. */
static __thread uint32_t _sel_rotation;

static size_t _sel_rotation_start(size_t n) {
  if (n < 2) return 0;
  _sel_rotation += 0x9E3779B9u;
  return (size_t)(((uint64_t)_sel_rotation * (uint64_t)n) >> 32);
}

/* Turns the events a poll of this call returned into fs->fd_ready: the flag
 * of every fd selectable whose own direction is satisfied by an event on its
 * fd gets set, and every other flag is cleared. A queue eventfd event sets
 * nothing, since Phase 1 reads each queue's state itself, and neither does
 * an event that refines to no bit (on kqueue, an EOF of the receive
 * direction that only a write direction is watching). Returns true when any
 * flag is set. */
static bool _sel_mark_fd_events(_sel_fd_state *fs, size_t n,
                                ccol_selectable *selectables, int n_ev) {
  memset(fs->fd_ready, 0, n * sizeof(bool));
  bool any = false;
  for (int e = 0; e < n_ev; e++) {
    ccol_poll_event *ev = &fs->evs[e];
    if (!(ev->data.u64 & _SEL_FD_TAG)) continue;
    size_t run = (size_t)(ev->data.u64 & ~_SEL_FD_TAG);
    int fd = fs->pairs[run].fd;
    size_t end = run;
    uint32_t combined = 0;
    while (end < fs->n_pairs && fs->pairs[end].fd == fd) {
      combined |= _sel_fd_base_events(selectables[fs->pairs[end].idx].dir);
      end++;
    }
    /* The caller owns the fd for the whole call, so it's still open. */
    ccol_poll_refine(ev, fd, combined, NULL);
    for (size_t j = run; j < end; j++) {
      size_t idx = fs->pairs[j].idx;
      if (_sel_fd_base_events(selectables[idx].dir) & ev->events) {
        fs->fd_ready[idx] = true;
        any = true;
      }
    }
  }
  return any;
}

static int _sel_fd_pair_cmp(const void *a, const void *b) {
  const _sel_fd_pair *pa = (const _sel_fd_pair *)a;
  const _sel_fd_pair *pb = (const _sel_fd_pair *)b;
  if (pa->fd != pb->fd) return (pa->fd < pb->fd) ? -1 : 1;
  if (pa->idx != pb->idx) return (pa->idx < pb->idx) ? -1 : 1;
  return 0;
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. When not 0, ccol_select_timed()'s allocation with that number,
 * counting from 1 when it was armed, fails and the hook disarms itself; see
 * _ccol_select_test_fail_alloc. */
static _Atomic uint32_t g_sel_fail_alloc_at = 0;
/* Test-only. Counts the epoll sets ccol_select_timed() creates; see
 * _ccol_select_test_poll_sets_created. */
static _Atomic uint64_t g_sel_poll_sets_created = 0;

void _ccol_select_test_fail_alloc(uint32_t index) {
  atomic_store(&g_sel_fail_alloc_at, index);
}

uint64_t _ccol_select_test_poll_sets_created(void) {
  return atomic_load(&g_sel_poll_sets_created);
}
#endif

/* Every allocation ccol_select_timed() makes goes through here. */
static void *_sel_malloc(size_t size) {
#ifdef RUNNING_UNIT_TESTS
  uint32_t at = atomic_load(&g_sel_fail_alloc_at);
  while (at != 0) {
    if (atomic_compare_exchange_weak(&g_sel_fail_alloc_at, &at, at - 1)) {
      if (at == 1) return NULL;
      break;
    }
  }
#endif
  return malloc(size);
}

/* Fills *fs with the array's fd selectables in array order and allocates
 * what Phase 1 needs, one block for pairs, pfds and fd_ready. Whatever the
 * result, the caller frees it with _sel_fd_state_free. Returns ccol_success
 * or ccol_not_enough_memory. */
static ccol_retval_t _sel_fd_state_init(size_t n, ccol_selectable *selectables,
                                        _sel_fd_state *fs) {
  fs->pairs = NULL;
  fs->pfds = NULL;
  fs->evs = NULL;
  fs->ev_cap = 0;
  fs->fd_ready = NULL;
  fs->n_pairs = 0;
  size_t n_fd = 0;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == ccol_selectable_fd) n_fd++;
  }
  fs->has_queues = n_fd < n;

  /* These guard the sizes below against size_t overflow. _sel_validate_args'
   * n * sizeof(ccol_sel_waiter) and n <= INT_MAX guards already make them
   * unreachable (n_fd <= n and no element here is larger than a
   * ccol_sel_waiter), but the block keeps its own explicit check where it is
   * sized. The pollfd array follows the pairs, whose alignment is at least
   * its own. */
  _Static_assert(_Alignof(_sel_fd_pair) % _Alignof(struct pollfd) == 0,
                 "the pollfd array must be aligned after the pairs");
  if (n_fd > SIZE_MAX / (sizeof(_sel_fd_pair) + sizeof(struct pollfd)) ||
      n > SIZE_MAX - n_fd * (sizeof(_sel_fd_pair) + sizeof(struct pollfd)))
    return ccol_not_enough_memory;
  size_t pfds_off = n_fd * sizeof(_sel_fd_pair);
  size_t ready_off = pfds_off + n_fd * sizeof(struct pollfd);
  unsigned char *block = _sel_malloc(ready_off + n * sizeof(bool));
  if (!block) return ccol_not_enough_memory;
  fs->pairs = (_sel_fd_pair *)(void *)block;
  fs->pfds = (struct pollfd *)(void *)(block + pfds_off);
  fs->fd_ready = (bool *)(void *)(block + ready_off);

  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type != ccol_selectable_fd) continue;
    fs->pairs[k].fd = selectables[i].fd;
    fs->pairs[k].idx = i;
    fs->pfds[k].fd = selectables[i].fd;
    fs->pfds[k].events =
        (selectables[i].dir == ccol_select_read) ? POLLIN : POLLOUT;
    fs->pfds[k].revents = 0;
    k++;
  }
  fs->n_pairs = k;
  return ccol_success;
}

/* Asks poll(2), without waiting, which fd selectables are ready and records
 * the answer in fs->fd_ready. A read selectable is ready on POLLIN (data or
 * an end of file), a write selectable on POLLOUT, and both on POLLERR and
 * POLLHUP, the same conditions the epoll set reports for them. An fd naming
 * a regular file or a directory makes the call fail with EPERM, as the epoll
 * set would refuse it; poll(2) reports such a file ready in every direction,
 * so only a ready fd needs the fstat(2) that tells us. An fd that isn't open
 * fails with EBADF. Returns 0, or -1 with errno set. */
static int _sel_poll_fds(_sel_fd_state *fs, size_t n,
                         ccol_selectable *selectables) {
  int r;
  do {
    r = poll(fs->pfds, (nfds_t)fs->n_pairs, 0);
  } while (r < 0 && errno == EINTR);
  if (r < 0) return -1;
  memset(fs->fd_ready, 0, n * sizeof(bool));
  if (r == 0) return 0;
  for (size_t k = 0; k < fs->n_pairs; k++) {
    short rev = fs->pfds[k].revents;
    if (rev == 0) continue;
    if (rev & POLLNVAL) {
      errno = EBADF;
      return -1;
    }
    size_t idx = fs->pairs[k].idx;
    short want = (selectables[idx].dir == ccol_select_read) ? POLLIN : POLLOUT;
    if (!(rev & (want | POLLERR | POLLHUP))) continue;
    struct stat st;
    if (fstat(fs->pairs[k].fd, &st) != 0) return -1;
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
      errno = EPERM;
      return -1;
    }
    fs->fd_ready[idx] = true;
  }
  return 0;
}

/* Builds what a call with fd selectables needs in order to block: an epoll
 * instance holding every fd selectable with level-triggered interest flags,
 * the event buffer, and an eventfd for each queue selectable's waiter node.
 * A call whose Phase 1 finds a ready selectable never gets here, so it
 * creates no epoll instance and no eventfd. Every node must be unlinked when
 * this runs.
 *
 * When several selectables share an fd, say a caller watching one connected
 * socket for both readability and writability in a single call, or two exact
 * duplicates, all of them are combined into a SINGLE epoll_ctl registration
 * carrying the OR of every member's interest mask, the same way
 * ccol_event_loop_add folds a read and a write registration on one fd into
 * one entry. Without that, a second EPOLL_CTL_ADD for an fd an earlier step
 * of the same call registered fails with EEXIST. That isn't the caller's
 * fault: nothing in the public API forbids it or calls it unsupported, and
 * it's an ordinary request, wait for whichever direction on one fd comes
 * first.
 *
 * The selectables are grouped by fd with an O(n log n) sort instead of an
 * O(n) linear scan per "is this fd already registered" lookup, which keeps
 * the common case of many distinct, unshared fds away from O(n^2). We treat
 * reducible algorithmic complexity as a bug even when today's typical n is
 * small. The pollfd array is rebuilt in sorted order, so pfds[k] still
 * watches pairs[k].
 *
 * On success *epfd is the epoll instance, and closing it removes every
 * registered fd automatically. Returns ccol_success, ccol_not_enough_memory
 * when the event buffer can't be allocated, or ccol_unexpected_failure on a
 * system error, after which *epfd is either -1 or an instance the caller
 * closes. */
static ccol_retval_t _sel_setup_epoll(size_t n, ccol_selectable *selectables,
                                      ccol_sel_waiter *nodes, _sel_fd_state *fs,
                                      int *epfd) {
  size_t ev_cap = n + fs->n_pairs;
  if (ev_cap > (size_t)INT_MAX) ev_cap = (size_t)INT_MAX;
  if (ev_cap > SIZE_MAX / sizeof(ccol_poll_event))
    return ccol_not_enough_memory;
  fs->evs = _sel_malloc(ev_cap * sizeof(ccol_poll_event));
  if (!fs->evs) return ccol_not_enough_memory;
  fs->ev_cap = (int)ev_cap;

  *epfd = ccol_poll_create();
  if (*epfd < 0) return ccol_unexpected_failure;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_sel_poll_sets_created, (uint64_t)1);
#endif

  size_t n_pairs = fs->n_pairs;
  qsort(fs->pairs, n_pairs, sizeof(_sel_fd_pair), _sel_fd_pair_cmp);
  for (size_t k = 0; k < n_pairs; k++) {
    size_t idx = fs->pairs[k].idx;
    fs->pfds[k].fd = fs->pairs[k].fd;
    fs->pfds[k].events =
        (selectables[idx].dir == ccol_select_read) ? POLLIN : POLLOUT;
    fs->pfds[k].revents = 0;
  }

  for (size_t i = 0; i < n_pairs;) {
    int fd = fs->pairs[i].fd;
    uint32_t combined = 0;
    size_t j = i;
    while (j < n_pairs && fs->pairs[j].fd == fd) {
      combined |= _sel_fd_base_events(selectables[fs->pairs[j].idx].dir);
      j++;
    }

    ccol_poll_event ev;
    ev.data.u64 = _SEL_FD_TAG | (uint64_t)i;
    ev.events = combined;
    if (ccol_poll_ctl(*epfd, CCOL_POLL_CTL_ADD, fd, &ev, NULL) < 0)
      return ccol_unexpected_failure;
    i = j;
  }

  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type != ccol_selectable_fd &&
        !_sel_ensure_efd(i, nodes, *epfd))
      return ccol_unexpected_failure;
  }
  return ccol_success;
}

/* Scans the selectables in rotation order, from index start to n - 1 and
 * then from 0 to start - 1, and stops at the first ready one. A queue
 * selectable that isn't ready gets its waiter node linked into the queue's
 * list, with whatever eventfd the node has (none until the call first
 * prepares to block). When the set holds fds, one zero-timeout poll(2) runs
 * first (_sel_poll_fds), and an fd selectable it reports ready takes part in
 * the scan like a ready queue. The rotation is what gives every ready
 * selectable its chance; a scan that always started at 0 would hand every
 * call to the first ready queue for as long as it stayed ready. Returns the
 * index found (0 or more), -1 when nothing is ready and every queue waiter is
 * registered, and -2 on a system error from the poll, before any node gets
 * linked. fs is NULL when the set holds no fd. */
static int _sel_phase1_scan_register(size_t n, ccol_selectable *selectables,
                                     ccol_sel_waiter *nodes, size_t start,
                                     _sel_fd_state *fs, _ccol_mutex_t *sel_mtx,
                                     _ccol_cond_var_t *sel_cond, bool *ready) {
  bool has_fd_sels = fs != NULL;
  if (has_fd_sels && _sel_poll_fds(fs, n, selectables) != 0) return -2;
  int found = -1;
  size_t i = start;
  for (size_t left = n; left != 0 && found < 0;
       left--, i = (i + 1 == n) ? 0 : i + 1) {
    if (selectables[i].type == ccol_selectable_fd) {
      if (has_fd_sels && fs->fd_ready[i]) found = (int)i;
      continue;
    }

    if (selectables[i].type == ccol_selectable_circq) {
      ccol_circular_queue *cq = selectables[i].cq;
      _ccol_mutex_lock(cq->mutex);

      if (selectables[i].dir == ccol_select_read) {
        /* Only a peek: ccol_select() never consumes a message, the caller
         * does its own ccol_circq_try_recv_zc() after the call. We forward
         * the notify to the next waiter every time, like the write branch
         * below, and the forward doesn't depend on messages left after a
         * consume because this function never consumes. A thread about to
         * leave the waiter list has no other way to tell the next waiter
         * that the condition is true. Without the forward the read direction
         * would have the same kind of starvation that
         * write_circq_two_concurrent_waiters_both_wake_on_slot_free guards
         * against on the write side. */
        if (cq->msg_count > 0) {
          notify_one_sel_waiter(&cq->sel_read_waiters_head,
                                &cq->sel_read_rotor);
          _ccol_mutex_unlock(cq->mutex);
          found = (int)i;
        } else {
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &cq->sel_read_waiters_head);
          _ccol_mutex_unlock(cq->mutex);
        }
      } else {
        /* ccol_select_write is ready when there's room or sends are turned
         * off; in the latter case a send reports ccol_not_permitted at once,
         * like a write to a pipe whose reader is gone reports EPIPE. */
        if (cq->msg_count < cq->max_size || cq->writing_disabled) {
          notify_one_sel_waiter(&cq->sel_write_waiters_head,
                                &cq->sel_write_rotor);
          _ccol_mutex_unlock(cq->mutex);
          found = (int)i;
        } else {
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &cq->sel_write_waiters_head);
          _ccol_mutex_unlock(cq->mutex);
        }
      }
    } else {
      /* ccol_selectable_dynq */
      ccol_dynamic_queue *dq = selectables[i].dq;
      _ccol_mutex_lock(dq->mutex);

      if (selectables[i].dir == ccol_select_read) {
        /* Only a peek, for the same reason as in the circq read branch
         * above. */
        if (dq->msg_count > 0) {
          notify_one_sel_waiter(&dq->sel_read_waiters_head,
                                &dq->sel_read_rotor);
          _ccol_mutex_unlock(dq->mutex);
          found = (int)i;
        } else {
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &dq->sel_read_waiters_head);
          _ccol_mutex_unlock(dq->mutex);
        }
      } else {
        /* ccol_select_write is ready when the queue isn't at capacity or
         * sends are turned off, as in the circq branch above. */
        if (dq->writing_disabled || dq->msg_count < ccol_max_elem_count) {
          notify_one_sel_waiter(&dq->sel_write_waiters_head,
                                &dq->sel_write_rotor);
          _ccol_mutex_unlock(dq->mutex);
          found = (int)i;
        } else {
          _sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                           &dq->sel_write_waiters_head);
          _ccol_mutex_unlock(dq->mutex);
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
  _SEL_CONDVAR_FAILURE,   /* _ccol_cond_var_timedwait returned an error that is
                            not 0 and not ETIMEDOUT */
} _sel_condvar_outcome;

#ifdef RUNNING_UNIT_TESTS
/* Test-only hook. It makes the next _ccol_cond_var_timedwait call in
 * _sel_wait_condvar's deadline branch report EINVAL without really waiting,
 * then disarms itself. It's there so a test can reach, every time, the path
 * where _ccol_cond_var_timedwait returns an unexpected error (not 0 and not
 * ETIMEDOUT). No correct call in this codebase can produce a real EINVAL
 * here, since _sel_compute_deadline always keeps tv_nsec in [0, 1e9) before
 * the deadline goes to _ccol_cond_var_timedwait. */
static _Atomic bool g_sel_force_condvar_wait_error = false;

/* Test-only hook read together with g_sel_force_condvar_wait_error. When
 * both fire, the injected error also sets *ready = true right before the
 * error is reported. That simulates a producer finishing its notify, under
 * the same sel_mtx this call already holds, a moment before we see the
 * unrelated forced error, and it's the only way a test can get that
 * interleaving every time. The hook replaces the real
 * _ccol_cond_var_timedwait call, so sel_mtx is never unlocked, and since a
 * real concurrent producer's _notify_waiter needs sel_mtx it could never
 * race in here naturally. The FAILURE-branch comment of _sel_wait_condvar
 * explains why *ready must win over an unexpected error arriving at the same
 * moment. */
static _Atomic bool g_sel_force_condvar_wait_error_also_marks_ready = false;

/* Test-only hook. Each armed count makes one _sel_wait_epoll wait that asks
 * for the largest timeout (INT_MAX milliseconds) return 0 at once, the way
 * the real wait does when that capped time runs out ahead of a later
 * deadline. */
static _Atomic uint32_t g_sel_short_capped_waits = 0;

void _ccol_select_test_short_next_capped_waits(uint32_t count) {
  atomic_store(&g_sel_short_capped_waits, count);
}

static int _sel_poll_wait_for_tests(int epfd, ccol_poll_event *evs, int cap,
                                    int timeout_ms) {
  if (timeout_ms == INT_MAX) {
    uint32_t left = atomic_load(&g_sel_short_capped_waits);
    while (left > 0) {
      if (atomic_compare_exchange_weak(&g_sel_short_capped_waits, &left,
                                       left - 1))
        return 0;
    }
  }
  return ccol_poll_wait(epfd, evs, cap, timeout_ms);
}

void _ccol_select_test_force_next_condvar_wait_error(void) {
  atomic_store(&g_sel_force_condvar_wait_error, true);
}

void _ccol_select_test_force_next_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_sel_force_condvar_wait_error, true);
  atomic_store(&g_sel_force_condvar_wait_error_also_marks_ready, true);
}

/* Test-only hook that widens the window between ccol_select_timed's
 * condvar-only wait reporting its deadline elapsed and the same call's
 * Phase 3 deregister step after it. The window is normally a few
 * instructions and the hook stretches it to whatever the test needs. A
 * producer's notify that lands right in that window targets a waiter that
 * has stopped waiting, which is the interleaving the forward in
 * deregister_sel_waiter exists for, and in production the window is far too
 * narrow to hit on demand. The hook fires once and then disarms itself, and
 * it keeps a counter the test polls to know the delay really started
 * instead of guessing with a sleep. */
static _Atomic uint32_t g_sel_timed_out_deregister_delay_us = 0;
static _Atomic uint64_t g_sel_timed_out_deregister_delays = 0;

void _ccol_select_test_delay_next_timed_out_deregister_us(uint32_t us) {
  atomic_store(&g_sel_timed_out_deregister_delay_us, us);
}

uint64_t _ccol_select_test_timed_out_deregister_delay_count(void) {
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

/* Waits on sel_cond until a producer sets *ready or the deadline passes. It
 * returns _SEL_CONDVAR_TIMED_OUT when the deadline passed with *ready still
 * false, _SEL_CONDVAR_FAILURE when _ccol_cond_var_timedwait returned an
 * error other than ETIMEDOUT with *ready still false, and _SEL_CONDVAR_READY
 * on a normal wake. A wait with no end always ends in _SEL_CONDVAR_READY, and
 * a wait with a deadline also gives _SEL_CONDVAR_READY when *ready is
 * already true at the moment we see ETIMEDOUT or an unexpected error. *ready
 * is always set back to false before returning.
 *
 * The FAILURE case matches every other timed wait in this file:
 * ccol_circq_timed_send_zc, ccol_circq_timed_recv_zc and
 * ccol_dynmq_timed_recv_zc all treat a _ccol_cond_var_timedwait return that
 * is neither 0 nor ETIMEDOUT as ccol_unexpected_failure, and none of them
 * retries. Without a separate exit for that error the loop's only exits are
 * ETIMEDOUT and *ready, so such an error would go back into
 * _ccol_cond_var_timedwait with the same deadline and the loop would spin
 * for ever without returning to the caller.
 *
 * Like the ETIMEDOUT branch right above it, the FAILURE branch looks at
 * *ready again before it commits. _ccol_cond_var_timedwait always takes
 * *sel_mtx again before returning, on success and on failure, and a
 * producer's _notify_waiter needs *sel_mtx to set *ready = true, so the
 * producer can finish and hand the mutex back to this very call a moment
 * before we also see some unrelated spurious error that isn't ETIMEDOUT.
 * Without the second check we'd throw away a real wake and report
 * ccol_unexpected_failure although the operation actually succeeded. */
static _sel_condvar_outcome _sel_wait_condvar(_ccol_mutex_t *sel_mtx,
                                              _ccol_cond_var_t *sel_cond,
                                              bool *ready, bool has_deadline,
                                              const struct timespec *deadline) {
  _ccol_mutex_lock(*sel_mtx);
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
        wait_ret = _ccol_cond_var_timedwait(*sel_cond, *sel_mtx, *deadline);
      }
#else
      wait_ret = _ccol_cond_var_timedwait(*sel_cond, *sel_mtx, *deadline);
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
    _ccol_mutex_unlock(*sel_mtx);
    return outcome;
  }
  while (!*ready) _ccol_cond_var_wait(*sel_cond, *sel_mtx);
  *ready = false;
  _ccol_mutex_unlock(*sel_mtx);
  return _SEL_CONDVAR_READY;
}

typedef enum {
  _SEL_EPOLL_CONTINUE, /* A queue eventfd fired. Go on to Phase 3. */
  _SEL_EPOLL_BREAK,    /* Done or timed out. *out_retval and *ready_index are
                          set. */
  _SEL_EPOLL_FAILURE,  /* An unexpected system error. The nodes stay
                          registered. */
} _sel_epoll_outcome;

/* Blocks on epoll_wait until a registered descriptor is ready or the
 * deadline passes. It returns _SEL_EPOLL_CONTINUE when no fd selectable is
 * ready after the wait (a queue eventfd fired, or an fd event refined to no
 * bit), and the caller then runs Phase 3 and goes back to Phase 1. It
 * returns _SEL_EPOLL_BREAK when the whole result is known, having set
 * *out_retval and *ready_index itself, and _SEL_EPOLL_FAILURE on a system
 * error, in which case the nodes stay registered and the caller must
 * deregister them before freeing them. A single wait returns every ready
 * registration and the ready fd selectable first in rotation order from
 * start wins, so a set's fds share the calls the same way its queues do. */
static _sel_epoll_outcome _sel_wait_epoll(
    int epfd, bool has_deadline, const struct timespec *deadline, size_t n,
    ccol_selectable *selectables, ccol_sel_waiter *nodes, _sel_fd_state *fs,
    size_t start, size_t *ready_index, ccol_retval_t *out_retval) {
  int n_ready;
  int epoll_to;
  for (;;) {
    if (has_deadline) {
      /* The remaining time rounds UP to a whole millisecond so the wait never
       * ends before the deadline. Once the deadline has passed the timeout
       * is 0 and epoll_wait still runs, which reports a descriptor that is
       * already ready (an epoll timeout of 0 is a poll that doesn't block,
       * not a timeout without a look). The remaining time is capped at
       * INT_MAX milliseconds (about 24.8 days), so one wait can end well
       * before a later deadline. */
      epoll_to = ccol_deadline_remaining_ms_ceil(deadline);
    } else {
      epoll_to = -1;
    }
#ifdef RUNNING_UNIT_TESTS
    n_ready = _sel_poll_wait_for_tests(epfd, fs->evs, fs->ev_cap, epoll_to);
#else
    n_ready = ccol_poll_wait(epfd, fs->evs, fs->ev_cap, epoll_to);
#endif
    if (n_ready < 0 && errno == EINTR) continue;
    /* A wait that reports nothing is a timeout only if the deadline has
     * passed. A wait the INT_MAX cap cut short, or one with no time limit,
     * just waits again for whatever time is left. */
    if (n_ready == 0 && epoll_to != 0 &&
        (!has_deadline || ccol_deadline_remaining_ms_ceil(deadline) > 0))
      continue;
    break;
  }

  if (n_ready == 0) {
    deregister_all_sel_waiters(n, nodes, selectables);
    *out_retval = ccol_timed_out;
    return _SEL_EPOLL_BREAK;
  }
  if (n_ready < 0) return _SEL_EPOLL_FAILURE;

  /* A wait that reported only queue eventfds, or only fd events matching no
   * member, goes on to Phase 3, which looks at the queues again. */
  if (!_sel_mark_fd_events(fs, n, selectables, n_ready))
    return _SEL_EPOLL_CONTINUE;
  size_t match = n;
  for (size_t step = 0; step < n; step++) {
    size_t k = start + step;
    if (k >= n) k -= n;
    if (fs->fd_ready[k]) {
      match = k;
      break;
    }
  }
  deregister_all_sel_waiters(n, nodes, selectables);

  /* Only readiness is reported, in both directions. ccol_select() never
   * reads from or writes to the fd, the caller does its own read(2),
   * recv(2), write(2) or send(2) after the call. */
  *out_retval = ccol_success;
  *ready_index = match;
  return _SEL_EPOLL_BREAK;
}

/* Blocks until at least one of the n selectables is ready or timeout_us
 * passes, then sets *ready_index. ccol_select_timed() never does the receive
 * or the send itself for any type of selectable; the caller does that after
 * the call. A timeout_us of UINT64_MAX means a wait with no end.
 *
 * The lock protocol is what prevents both a deadlock and a lost wake. Each
 * iteration goes through three phases.
 *
 *   Phase 1: in rotation order from a start index that changes on every
 *   call (see _sel_rotation_start), up to the first ready selectable, and for
 *   each queue under that queue's mutex, one at a time:
 *     Read direction: check msg_count > 0, a peek that consumes nothing. If
 *       the queue is ready, forward the notify to the next read waiter (like
 *       the write-direction cascade below) and mark the selectable found.
 *       If not, prepend a waiter node to the queue's sel_read_waiters_head
 *       list and wait. The read direction waits whatever writing_disabled
 *       says.
 *     Write direction: for a ccol_circular_queue check
 *       msg_count < max_size || writing_disabled, for a ccol_dynamic_queue
 *       writing_disabled || msg_count < ccol_max_elem_count. A queue whose
 *       sends are off counts as ready, because the caller's send then
 *       returns ccol_not_permitted at once, just as a write to a pipe whose
 *       reader is gone fails at once. If the queue is ready, mark the
 *       selectable found first, reserving and consuming nothing. If
 *       not, prepend a waiter node to sel_write_waiters_head and wait.
 *     When the set holds fds, Phase 1 starts with one non-blocking poll(2)
 *       of every fd selectable, and an fd it reports ready in its
 *       selectable's direction is ready in the scan just like a ready queue.
 *       The first ready selectable in rotation order wins, so a queue that
 *       stays ready can't hide a ready fd or another ready queue. A call
 *       that finds a ready selectable here makes no other system call for
 *       its fds.
 *     The first time a call with fds finds nothing ready, it unlinks its
 *       nodes, builds the epoll set (every fd selectable plus an eventfd for
 *       each queue selectable's node) and runs Phase 1 again, so every node
 *       is linked with its eventfd before the wait.
 *
 *   Phase 2: the wait:
 *     With no fd selectable, the thread blocks on sel_cond under sel_mtx.
 *       The while loop guards against spurious wakes and against a signal
 *       that fired between Phase 1 and the wait; a producer sets the ready
 *       flag under sel_mtx before signalling, so no wake can be lost. With a
 *       deadline we use _ccol_cond_var_timedwait on a CLOCK_MONOTONIC
 *       condition variable, and ETIMEDOUT breaks the loop so the function
 *       returns ccol_timed_out.
 *     With an fd selectable, the thread blocks on epoll_wait(epfd). Each
 *       queue waiter has its own eventfd (efd >= 0) and a producer's notify
 *       (_notify_waiter) writes 1 to it, so epoll_wait wakes for queue
 *       events as well as fd events. If a user fd fires, the function
 *       returns the ready fd selectable that comes first in rotation order,
 *       and if only a queue eventfd fired we go on to Phase 3 and look at
 *       the state again. With a deadline the remaining time is recomputed
 *       before every epoll_wait call, after an EINTR retry too, so EINTR
 *       can't stretch the timeout. An epoll_wait returning 0 after the
 *       deadline makes the function return ccol_timed_out, while one
 *       returning 0 before it (a single wait is capped at INT_MAX
 *       milliseconds) waits again.
 *
 *   Phase 3: the deregister step:
 *     Take each queue's mutex again and splice the node out of the right
 *     waiter list, forwarding the wake to the next waiter if that queue is
 *     still ready (see deregister_sel_waiter). Drain each open eventfd with a
 *     non-blocking read, which resets its counter to 0 for the next
 *     iteration, and go back to Phase 1.
 *
 * The lock order is queue mutex first and sel_mtx second, and every path in
 * this file uses that one order: a producer takes it to notify, Phase 1
 * when a ready queue forwards its wake, and Phase 3 when a node leaves.
 * Phase 2's condvar path takes sel_mtx alone and never a queue mutex under
 * it, and epoll_wait holds no application lock, so there is no cycle in the
 * lock order.
 *
 * Node lifetime: a producer notifies while holding the queue mutex and the
 * deregister step needs the queue mutex too, so a node can't leave the list
 * while a producer is walking it, and the heap node can't disappear in the
 * middle of a walk.
 *
 * The epfd lifecycle: the epfd is created at most once per call, the first
 * time the call has to block, and each user fd and queue eventfd is added
 * once with EPOLL_CTL_ADD and stays registered for the rest of the call.
 * With level-triggered semantics a ready fd fires on every epoll_wait until
 * the caller consumes it, and closing epfd on return removes everything
 * automatically, so there is no EPOLL_CTL_DEL or EPOLL_CTL_ADD per
 * iteration. deregister_sel_waiter drains the queue eventfds with a
 * non-blocking read instead of closing them, so Phase 1 links the same node
 * again without a new epoll_ctl or eventfd syscall. */
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

  /* One waiter node per selectable. sel_mtx == NULL means the node isn't in
   * any queue's list right now, and efd == -1 means it has no eventfd (the
   * condvar-only mode). The array lives on the heap so a large n can't
   * overflow the stack.
   */
  ccol_sel_waiter *nodes = _sel_malloc(n * sizeof(ccol_sel_waiter));
  if (!nodes) return ccol_not_enough_memory;
  for (size_t i = 0; i < n; i++) {
    nodes[i].sel_mtx = NULL;
    nodes[i].efd = -1;
  }

  _ccol_mutex_t sel_mtx;
  _ccol_cond_var_t sel_cond;
  bool ready = false;
  if (_ccol_mutex_init(sel_mtx) != 0) {
    free(nodes);
    return ccol_unexpected_failure;
  }
  /* CLOCK_MONOTONIC, like every other condition variable a timed wait in
   * this file blocks on (see _init_monotonic_cond_var). */
  if (_init_monotonic_cond_var(&sel_cond) != 0) {
    _ccol_mutex_destroy(sel_mtx);
    free(nodes);
    return ccol_unexpected_failure;
  }

  bool has_deadline;
  struct timespec deadline = {0, 0};
  has_deadline = _sel_compute_deadline(timeout_us, &deadline);

  int epfd = -1;
  _sel_fd_state fd_state;
  _sel_fd_state *fs = NULL;
  if (has_fd_sels) {
    fs = &fd_state;
    if (_sel_fd_state_init(n, selectables, fs) != ccol_success) {
      _sel_fd_state_free(fs);
      free(nodes);
      _ccol_mutex_destroy(sel_mtx);
      _ccol_cond_var_destroy(sel_cond);
      return ccol_not_enough_memory;
    }
  }
  ccol_retval_t failure = ccol_unexpected_failure;

  /* One start index for the whole call, so a wake that sends us back to
   * Phase 1 keeps the same order. */
  size_t start = _sel_rotation_start(n);

  ccol_retval_t retval = ccol_success;

  for (;;) {
    /* === Phase 1: scan + register === */
    int found = _sel_phase1_scan_register(n, selectables, nodes, start, fs,
                                          &sel_mtx, &sel_cond, &ready);
    if (found == -2) goto cleanup_failure;
    if (found >= 0) {
      deregister_all_sel_waiters(n, nodes, selectables);
      *ready_index = (size_t)found;
      retval = ccol_success;
      break;
    }

    /* The first time a call with fds has to block it builds the epoll set
     * and its queue waiters' eventfds, then scans again, so every queue
     * waiter is linked with its eventfd and a wake that arrived in between
     * isn't lost. A call answered at once by a ready selectable never gets
     * here. */
    if (has_fd_sels && epfd < 0) {
      deregister_all_sel_waiters(n, nodes, selectables);
      /* Phase 1's poll ran after the deadline, so a call whose time is up has
       * already had its last look and needs no epoll set for a wait of 0. */
      if (has_deadline && ccol_deadline_remaining_ms_ceil(&deadline) == 0) {
        retval = ccol_timed_out;
        break;
      }
      _ccol_mutex_lock(sel_mtx);
      ready = false;
      _ccol_mutex_unlock(sel_mtx);
      failure = _sel_setup_epoll(n, selectables, nodes, fs, &epfd);
      if (failure != ccol_success) goto cleanup_failure;
      failure = ccol_unexpected_failure;
      continue;
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
      if (outcome == _SEL_CONDVAR_FAILURE) goto cleanup_failure;
    } else {
      _sel_epoll_outcome outcome =
          _sel_wait_epoll(epfd, has_deadline, &deadline, n, selectables, nodes,
                          fs, start, ready_index, &retval);
      if (outcome == _SEL_EPOLL_BREAK) break;
      if (outcome == _SEL_EPOLL_FAILURE) goto cleanup_failure;
      /* _SEL_EPOLL_CONTINUE: go on to Phase 3 */
    }

    /* === Phase 3: deregister, then go back to Phase 1 ===
     * deregister_sel_waiter drains each open eventfd with a non-blocking
     * read, which resets its counter to 0. The eventfd stays registered in
     * epoll, so Phase 1 links the node again with no new syscall, and a user
     * fd stays registered in epoll for the same reason. */
    deregister_all_sel_waiters(n, nodes, selectables);
    /* No node is linked now, so no producer can reach ready or one of this
     * call's eventfds until Phase 1 links a node again. Whatever wake ready
     * recorded counts as consumed, since Phase 1 looks at every queue's state
     * again, and clearing it here is what lets the first notify after the
     * next link deliver a wake again. The condvar wait clears it itself, but
     * a notify landing between that wait's return and the deregister sets it
     * again, and in epoll mode nothing else would clear it at all. */
    _ccol_mutex_lock(sel_mtx);
    ready = false;
    _ccol_mutex_unlock(sel_mtx);
  }

  /* Close every eventfd the iterations allocated. Closing epfd takes every
   * user fd and queue eventfd out of the epoll set automatically. */
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].efd >= 0) close(nodes[i].efd);
  }
  if (epfd >= 0) close(epfd);
  if (fs) _sel_fd_state_free(fs);
  _ccol_mutex_destroy(sel_mtx);
  _ccol_cond_var_destroy(sel_cond);
  free(nodes);
  return retval;

cleanup_failure:
  deregister_all_sel_waiters(n, nodes, selectables);
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].efd >= 0) close(nodes[i].efd);
  }
  if (epfd >= 0) close(epfd);
  if (fs) _sel_fd_state_free(fs);
  _ccol_mutex_destroy(sel_mtx);
  _ccol_cond_var_destroy(sel_cond);
  free(nodes);
  return failure;
}

ccol_retval_t ccol_select(size_t *ready_index, size_t n,
                          ccol_selectable *selectables) {
  return ccol_select_timed(ready_index, n, selectables, UINT64_MAX);
}

/* From here on: ccol_event_loop. */

typedef struct event_entry event_entry;

/* The internal, mutable registration struct. The PUBLIC ccol_event_reg type
 * in cthreadcomm.h is something else: an opaque uint64_t handle that gets
 * resolved against this loop's reg_slots table before anything is
 * dereferenced (see _ccol_event_reg_resolve). The public type is never this
 * struct. */
typedef struct ccol_event_reg_s ccol_event_reg_s;
struct ccol_event_reg_s {
  ccol_selectable sel;
  ccol_event_handlers_t handlers;
  void *arg;
  _Atomic int refcount; /* 1 while registered, +1 for each live callback */
  _Atomic bool removed;

  /* True between a successful ccol_event_loop_pause and the matching
   * ccol_event_loop_resume. The registration stays fully intact (it keeps its
   * slot in owning_entry->as.fd.read_reg or owning_entry->as.fd.write_reg,
   * counts toward loop->reg_count and keeps its generation), but it isn't
   * part of the fd's combined epoll interest mask, so no callback fires for
   * it while this is true. _ccol_event_loop_rearm_entry_locked and
   * _ccol_event_loop_add_fd's first mask computation read it, and both
   * recompute the mask from live state rather than a cached snapshot, so
   * this flag is just one more bit of that live state, not a new mechanism.
   * It is always false for a queue or ccol_channel registration, since
   * ccol_event_loop_pause rejects those, and ccol_event_loop_modify has the
   * same restriction. */
  _Atomic bool paused;

  /* True once this fd registration got an event it has no handler for,
   * meaning neither its direction's handler nor on_error can take it. That's
   * usually a hang-up, which level-triggered epoll reports again on every
   * epoll_wait, so a registration that stays armed for it makes the reactor
   * spin. Like paused, the flag takes the registration out of the fd's
   * combined interest mask, while a sibling direction with handlers stays
   * armed. Collection sets it, ccol_event_loop_modify and
   * ccol_event_loop_resume clear it, and every read and write happens under
   * the registration's stripe lock. It fits in the padding after paused, so
   * the struct doesn't grow. */
  bool muted;

  event_entry *owning_entry;

  /* Which of loop->stripes holds this registration's entry. It's set once,
   * together with owning_entry, and never written again.
   * ccol_event_loop_modify and ccol_event_loop_remove must key their stripe
   * lookup on THIS field rather than owning_entry->stripe_idx, because a
   * caller can legitimately hold a stale reg*, exactly the case
   * _ccol_event_loop_defer_reg_free's contract exists for, and such a reg's
   * owning_entry may already be freed: entries and regs have separate
   * deferred-free lists that drain independently. The reg's own deferred-free
   * list is what keeps reg->stripe_idx always safe to read; the entry's list
   * is a different guarantee that doesn't cover a read through a stale
   * reg. */
  size_t stripe_idx;

  /* The identity token callers see through ccol_event_loop_reg_generation.
   * It's copied from owning_entry->generation at the same point as
   * stripe_idx and for the same reason: a stale reg* must never read it
   * through owning_entry, which can be freed independently. Written once by
   * that copy and never again, so like stripe_idx it's safe to read at any
   * time under the reg's deferred-free contract. */
  uint64_t generation;

  /* This registration's public ccol_event_reg handle, the value
   * ccol_event_loop_add returns. _ccol_event_reg_slot_acquire writes it once,
   * before reg is wired into any registry, so every dispatch reads it after
   * that write and none can see it unset. The dispatch hands it to every
   * on_readable, on_writable and on_error call, which lets a callback act on
   * its own registration even when it runs before ccol_event_loop_add has
   * returned the handle to its caller. */
  ccol_event_reg self_handle;

  /* This reg's index into loop->reg_slots (the comment of struct
   * _ccol_event_loop_s's reg_slots field explains the design). Set once
   * together with stripe_idx and generation and never written again. Only
   * ccol_event_loop_remove uses it, to invalidate the slot the moment it
   * removes this reg, since every OTHER lookup goes through the public
   * ccol_event_reg handle and never through this raw struct. */
  uint32_t self_slot_idx;

  /* _ccol_event_reg_resolve pins this count with an atomic increment while it
   * holds loop->reg_slot_rwlock's read side, and the pin stays for as long
   * as a caller holds a resolved ccol_event_reg_s* it hasn't released with
   * _ccol_event_reg_resolve_unpin yet. _ccol_event_loop_reclaim_pending_frees
   * never frees a reg while this count isn't zero, whatever its refcount. A
   * call on that exact reg (ccol_event_loop_modify, _pause, _resume, _remove
   * or ccol_event_loop_reg_generation) may be in flight, and this count is
   * what makes freeing the reg impossible for that whole call. The
   * poller_batch_gen epoch check alone leaves that gap open; see the
   * reg_slots comment of struct _ccol_event_loop_s. */
  _Atomic size_t pending_resolve_count;

  int bridge_efd; /* -1 for an fd selectable. For a queue or ccol_channel
                   * selectable it is the persistent bridge eventfd. */

  /* Queue and ccol_channel selectables only. _notify_waiter above records a
   * wake in a node's ready flag under that node's mutex, so a persistent
   * registration needs a mutex, condition variable and ready flag of its
   * own. wait_ready is true exactly while a wake is pending on bridge_efd.
   * _queue_reg_consume_wake clears it together with the drain right before a
   * dispatch runs the callback, so one eventfd write covers every send up to
   * that dispatch. Nothing ever waits on wait_cond, because a node with an
   * eventfd never gets a signal. */
  _ccol_mutex_t wait_mtx;
  _ccol_cond_var_t wait_cond;
  bool wait_ready;
  ccol_sel_waiter waiter_node;
  /* True once the loop's shutdown has unlinked waiter_node from its queue's
   * waiter list, so the remove and the teardown don't unlink it twice.
   * Guarded by the queue's mutex. */
  bool waiter_detached;

  /* Intrusive list of every queue-backed reg currently registered on this
   * registration's stripe. An fd-backed reg doesn't need one, since the
   * stripe's fd table reaches it, but a queue selectable has no such table to
   * walk, and __ccol_event_loop_destroy needs to find every queue-backed
   * waiter_node and unlink it from its queue's list before freeing it. */
  ccol_event_reg_s *loop_list_prev;
  ccol_event_reg_s *loop_list_next;

  /* Links this reg into loop->pending_reg_frees once its refcount reaches 0
   * (see _ccol_event_loop_defer_reg_free below). Nothing uses the reg again
   * after the free starts.
   */
  ccol_event_reg_s *pending_free_next;
};

/* ev.data.ptr points to this struct for every epoll registration this module
 * owns. For an fd, up to two event_regs (read_reg and write_reg) can share
 * one entry. epoll_ctl keys its interest list by fd, not by fd and
 * direction, so ev.data.ptr can't hold a bare ccol_event_reg* directly; if
 * it did, the reg of whichever direction registered last would silently get
 * every event on that fd, the other direction's included. A queue or
 * ccol_channel selectable never shares an entry: each gets its own bridge
 * eventfd and its own one-reg entry. */
struct event_entry {
  bool is_fd;
  int fd; /* For an fd selectable only. It is also the fd-table key. */

  /* Which of loop->stripes holds this entry. That stripe's lock protects
   * as.fd.read_reg, as.fd.write_reg and as.reg, and the stripe's fd_index and
   * queue_regs_head hold the entry. It's set once at creation, before the
   * entry is published (the insert into a stripe's chmap, or the epoll_ctl
   * call), and never written again, so every reader, the dispatch collector
   * reading through ev->data.ptr for one, can read it without a lock. Never
   * derive it later from entry->as.reg, entry->as.fd.read_reg or
   * entry->as.fd.write_reg outside a lock: ccol_event_loop_remove's queue
   * branch sets entry->as.reg to NULL while the poller may already hold a
   * stale epoll batch entry, and that NULL is what keeps the poller from
   * dereferencing a freed reg through it, so a stripe key derived from that
   * very field at dispatch time would defeat the protection. */
  size_t stripe_idx;

  /* This entry's dispatch lock. epoll is level-triggered by default and we
   * use no EPOLLEXCLUSIVE, so with more than one reactor thread two threads
   * can each get this same entry from their own concurrent epoll_wait while
   * it's still ready, the classic thundering herd that epoll itself doesn't
   * prevent. The lock is held across the entry's whole dispatch, both the
   * collection of read_reg and write_reg under the stripe lock below and the
   * callback call itself, while the stripe lock is held only for the
   * collection. That's what guarantees one registration's callback never
   * runs concurrently with itself. Both directions of an fd share this entry,
   * so a read and a write registration on the same fd never run at the same
   * time either. The strict guarantee is on purpose: each direction is
   * serialised against itself and the two against each other, so a caller
   * can share one resource, a TLS connection object say, across both
   * directions of an fd without a lock of its own.
   *
   * This is NOT the stripe lock. The stripe lock protects the registry (the
   * read_reg and write_reg slots and the fd_index chmap) against concurrent
   * ccol_event_loop_add, _remove and _modify calls from ANY thread, while
   * this lock protects one entry's dispatch against concurrent reactor
   * threads. No thread holding it tries to take a different entry's stripe
   * lock, and no path takes a stripe lock and then tries to take this one,
   * so it adds no cycle to the lock order. */
  _ccol_mutex_t dispatch_lock;

  /* The identity token callers see through ccol_event_loop_reg_generation.
   * It's minted once from loop->fd_generation_counter when the entry is
   * created, and an existing entry gaining its second direction gets no new
   * one. Every reg attaching to this entry copies it into reg->generation,
   * and a read through a stale reg must use the reg's copy, never
   * owning_entry->generation (the generation field of ccol_event_reg says
   * why). */
  uint64_t generation;

  /* Snapshot of loop->poller_batch_gen taken when this entry is pushed onto
   * loop->pending_entry_frees (see _ccol_event_loop_defer_entry_free). The
   * big comment above _ccol_event_loop_reclaim_pending_frees describes the
   * reclaim scheme it serves. It is 64 bits wide on every target, like
   * poller_batch_gen, so the defer_gen < poller_batch_gen test never meets a
   * wrap. */
  uint64_t defer_gen;

  /* True once ccol_event_loop_remove has fully unregistered this entry (no
   * direction and no reg left) and deferred it for freeing. Set exactly once,
   * in _ccol_event_loop_defer_entry_free, together with defer_gen.
   * _ccol_event_loop_rearm_entry_locked, the shared EPOLLONESHOT re-arm
   * helper, reads it to skip a real fd the application may already have
   * closed or reused before a dispatch job re-arms it. The generation
   * counter protects callers from that same hazard elsewhere, and this flag
   * brings the same protection to the module's own internal re-arm. */
  _Atomic bool removed;

  /* One higher while a dispatch job referencing this entry is queued for, or
   * running on, a ctpool worker, which only happens with
   * num_reactor_threads > 1; with num_reactor_threads == 1 no job is ever
   * created and the count stays 0. A job needs the entry alive for as long
   * as it runs, at least to lock and unlock entry->dispatch_lock and re-arm
   * the epoll interest afterwards, whether or not ccol_event_loop_remove has
   * fully unregistered the entry meanwhile.
   * _ccol_event_loop_reclaim_pending_frees doesn't free a deferred entry
   * while this count isn't zero, however far poller_batch_gen has moved:
   * the epoch check alone only proves that the batch the POLLER fetched can't
   * reference the entry any more, and says nothing about a ctpool worker job
   * that was submitted well after the collection and is still running. */
  _Atomic size_t refcount;

  union {
    struct {
      ccol_event_reg_s *read_reg;
      ccol_event_reg_s *write_reg;
    } fd;
    /* Queue or ccol_channel selectable: one reg per entry, never shared. */
    ccol_event_reg_s *reg;
  } as;

  /* fd selectables only. Says whether loop->epfd's interest set holds this
   * entry's fd right now. The kernel reports EPOLLERR and EPOLLHUP whatever
   * the registered mask is, even a mask of 0, so narrowing the mask alone can
   * never silence an fd whose every live direction is paused with
   * ccol_event_loop_pause. Without this flag, a paused fd that is (or gets
   * into) an error or hang-up state is reported over and over by a
   * level-triggered epoll_wait with nothing to show for it: the collection
   * fires and bumps the refcount, and only the callback is skipped because
   * _ccol_event_loop_run_callback checks reg->paused again. That's an
   * unbounded CPU spin on the poller thread, and with a dispatch_pool an
   * endless stream of allocate, submit, dequeue, skip and re-arm cycles. It
   * also breaks ccol_event_loop_pause's documented contract that no callback
   * fires, just as after a remove. A REMOVED registration gets no further
   * wake thanks to EPOLL_CTL_DEL, but a paused one with just a narrower mask
   * can't get that, since the mask can't opt out of ERR and HUP monitoring.
   *
   * When every live direction is paused, _ccol_event_loop_rearm_entry_locked
   * can end up with a mask with no real interest bit left, and the fd is then
   * taken out of loop->epfd's interest set with EPOLL_CTL_DEL. It's added
   * back once a direction resumes or a new one registers, with EPOLL_CTL_ADD
   * rather than EPOLL_CTL_MOD, since MOD on an fd that isn't registered at
   * the moment fails with ENOENT. This flag is how _ccol_event_loop_add_fd
   * and _ccol_event_loop_rearm_entry_locked pick the right epoll_ctl
   * operation. Like every other mutable field here it's read and written only
   * under the entry's stripe lock, so it needs no atomic access. It is always
   * false for queue and ccol_channel entries: their bridge eventfd is added
   * once, in _ccol_event_loop_add_queue, and this mechanism never removes or
   * re-adds it, because an eventfd has no always-on error condition like that
   * and a queue or ccol_channel registration can't be paused at all. */
  bool epoll_added;
  /* The CCOL_POLL_IN bit of the last mask an ADD or MOD of this fd used,
   * kept under the stripe lock like epoll_added. A MOD that flips it carries
   * CCOL_POLL_READ_MODE_CHANGE (see that flag in cpoll.h). */
  bool armed_in;
  /* What the readiness interface knows about an fd entry's fd, kept under
   * the stripe lock like epoll_added (see ccol_poll_fdinfo in cpoll.h). The
   * entry's calloc zeroes it. */
  ccol_poll_fdinfo poll_info;

  /* Links this entry into loop->pending_entry_frees when it is retired (see
   * _ccol_event_loop_defer_entry_free below). Nothing touches an entry again
   * after it's retired, which is why one field can safely carry both the
   * "live" and the "pending free" state. */
  event_entry *pending_free_next;
};

/* One independent group of a mutex, an fd index and a queue-reg list. A real
 * fd is handled by exactly one stripe for its whole life, and so is a queue
 * or ccol_channel registration's private bridge eventfd (see
 * _stripe_index_for_fd and the round-robin queue assignment in
 * ccol_event_loop_add), so no operation ever holds more than one
 * stripe's lock at once. */
typedef struct ccol_event_loop_stripe {
  _ccol_mutex_t lock;
  chmap fd_index; /* int fd -> event_entry*, for real fds only */
  ccol_event_reg_s *queue_regs_head; /* the regs in this stripe that a queue or
                                      * a ccol_channel backs */
} ccol_event_loop_stripe_t;

/* A ccol_event_loop is an opaque value handle with the slot index in its top
 * 32 bits and the generation in its bottom 32 bits (see the doc comment on
 * the typedef in include/cthreadcomm.h). The handle is resolved through this
 * table before the struct _ccol_event_loop_s* behind it is touched, which
 * lets __ccol_event_loop_destroy turn two cases into a _ccol_fatal_err
 * instead of a use-after-free or double free: a concurrent double destroy,
 * two destroys racing on the same live handle, and a sequential one, a stale
 * handle left over from an earlier destroy that has finished. A slot is
 * marked not in use the moment it's released and its generation goes up on
 * every reuse, so a stale handle can never name a later, unrelated loop in
 * the same slot. The table matches chttpcli_slot_table and
 * chttpsvr_slot_table; the copy of this comment in src/chttpclient.c gives the
 * reasoning.
 *
 * Its lock is a read-write lock, not a plain mutex. _ccol_event_loop_resolve
 * only reads (it bounds-checks the index, compares the generation and reads
 * slot->ptr) and it's the first step of ccol_event_loop_pause, _resume,
 * _modify, _remove and ccol_event_loop_reg_generation. A caller like
 * chttpserver pauses and resumes a connection's registration once per
 * request, so those calls run at the full request rate, while the only
 * writers, _ccol_event_loop_handle_slot_acquire and
 * __ccol_event_loop_destroy, run once per loop lifetime. It matches this
 * file's reg_slot_rwlock (see that field's comment on struct
 * _ccol_event_loop_s) and has the same fork-safety catch: a glibc write lock
 * records the writer's thread ID, and a fork() child has a new one. The doc
 * comment of _cthreadcomm_atfork_prepare describes that hazard and how we
 * deal with it. */
typedef struct {
  struct _ccol_event_loop_s *ptr; /* NULL when the slot is free */
  uint32_t generation;            /* minted fresh on every acquire. It rises for
                                      each slot index. It starts at 0 before the
                                      first use, and it becomes 1 on the first
                                      acquire. */
  bool in_use;
} ccol_event_loop_slot_t;

static struct {
  _ccol_rw_lock_t rwlock;
  _ccol_once_flag_t once;
  cvec slots;        /* A cvec of ccol_event_loop_slot_t. It grows only with
                         push_back, so an index is permanent after the
                         allocation. */
  cvec free_indices; /* A cvec of uint32_t. A LIFO free list, O(1) reuse */
  /* How many slot indices couldn't be pushed back onto free_indices because
     the push couldn't allocate. Such a slot is fully released (ptr NULL,
     in_use false) but nothing names it, so without this counter it would be
     unreachable for the rest of the process and every later create would
     grow the table by one more slot. An acquire that finds free_indices
     empty while this counter is above zero recovers one slot by scanning
     slots. The scan can't be confused, since it only runs while the free
     list is empty: a released slot is either on that list or lost, never
     both. Read and written only under the write lock. */
  size_t lost_indices;
  /* Set when the process-exit destructor finds a live loop and leaves this
     table alone; the destroy that later frees the last slot then does the
     release the destructor couldn't. Without it the table would be freed at
     exit no matter what, while another translation unit's destructor may
     still hold a loop, and the destroy it runs later would index a freed
     vector. */
  bool release_deferred;
} ccol_event_loop_slot_table = {0};

/* Defined further below, next to the process-exit destructor that sets the
   flag it reads, while its caller __ccol_event_loop_destroy comes earlier in
   this file. */
static void _release_event_loop_slot_table_if_deferred_locked(void);

/* How many slots the table holds; the caller holds either side of the lock.
 * Once the process-exit destructor has released the vectors they are NULL
 * and the table holds no slot, so a resolve finds nothing and a create fails
 * cleanly instead of indexing a vector that's gone. */
static inline size_t _event_loop_slot_count_locked(void) {
  return ccol_event_loop_slot_table.slots
             ? cvector_elem_count(ccol_event_loop_slot_table.slots)
             : 0;
}

/* Forward declarations. The bodies come after the declaration of struct
 * _ccol_event_loop_s further below, as they must, since they dereference
 * a live loop's shutdown_lock, reg_slot_rwlock and stripes[].
 * _cthreadcomm_register_atfork_once registers them. The doc comment of
 * queue_mutex_registry explains why this is ONE merged _ccol_at_fork()
 * triple, covering both ccol_event_loop_slot_table's locks and
 * queue_mutex_registry's queue mutexes, instead of two independent ones. */
#if CCOL_FORK_SAFETY_REQUIRED
static void _cthreadcomm_atfork_prepare(void);
static void _cthreadcomm_atfork_release(void);
static void _cthreadcomm_atfork_child_release(void);
#endif

static void _ccol_event_loop_slot_table_init_globals(void) {
  if (_ccol_rw_lock_init(ccol_event_loop_slot_table.rwlock) != 0)
    _ccol_fatal_err("ccol_event_loop slot table: failed to initialize rwlock");
  ccol_event_loop_slot_table.slots =
      cvector_create(sizeof(ccol_event_loop_slot_t), NULL);
  if (!ccol_event_loop_slot_table.slots)
    _ccol_fatal_err(
        "ccol_event_loop slot table: failed to allocate slots vector");
  ccol_event_loop_slot_table.free_indices =
      cvector_create(sizeof(uint32_t), NULL);
  if (!ccol_event_loop_slot_table.free_indices)
    _ccol_fatal_err(
        "ccol_event_loop slot table: failed to allocate free-index vector");
}

/* Registers the ONE shared _ccol_at_fork() triple covering
 * ccol_event_loop_slot_table's locks and queue_mutex_registry's queue
 * mutexes. The doc comment of queue_mutex_registry explains why it must be
 * one merged registration; two independent ones form an AB-BA deadlock
 * cycle.
 *
 * It first gets the plain data of BOTH structures (a mutex and one or two
 * cvecs, no atfork wiring) ready on first use, each through its own
 * _ccol_call_once, whichever structure triggered the call. A process may use
 * only a ccol_circular_queue or ccol_dynamic_queue and never touch
 * ccol_event_loop, and even then ccol_event_loop_slot_table's mutex and cvecs
 * must be initialised, because the merged prepare and release functions
 * registered here always walk ccol_event_loop_slot_table.
 *
 * fork() copies only the calling thread. The doc comment of
 * _cthreadcomm_atfork_prepare describes the full lock-inheritance hazard this
 * closes for parent and child, and the doc comment of
 * _cthreadcomm_atfork_child_release two more hazards that only the child has
 * and that a plain shared release function can't handle: joining a poller
 * thread that exists only in the parent, and the live kernel epoll object
 * the child shares with the parent. */
static void _cthreadcomm_register_atfork_once(void) {
#if CCOL_FORK_SAFETY_REQUIRED
  /* Both _ccol_call_once lines below stay inside this guard, which covers more
   * than just the _ccol_at_fork() registration. Every ccol_event_loop-side
   * caller of this function readies ccol_event_loop_slot_table itself with
   * its own adjacent _ccol_call_once(ccol_event_loop_slot_table.once, ...)
   * (see _ccol_event_loop_resolve and _ccol_event_loop_handle_slot_acquire
   * below), so the second line only matters for a queue-only caller,
   * _queue_mutex_registry_add and _queue_mutex_registry_remove. Those never
   * touch ccol_event_loop_slot_table.once directly, but they rely on the
   * merged atfork handlers below, which need the table ready. With this
   * macro at 0 no atfork machinery ever walks either registry, so neither
   * needs to be ready on this path at all. */
  _ccol_call_once(queue_mutex_registry.once,
                  _queue_mutex_registry_init_globals);
  _ccol_call_once(ccol_event_loop_slot_table.once,
                  _ccol_event_loop_slot_table_init_globals);
  _ccol_at_fork(_cthreadcomm_atfork_prepare, _cthreadcomm_atfork_release,
                _cthreadcomm_atfork_child_release);
#endif
}

#if defined(_CCOL_ATFORK_AT_LOAD)
/* See _CCOL_ATFORK_AT_LOAD in common.h. */
__attribute__((constructor)) static void _cthreadcomm_atfork_at_load(void) {
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
}
#endif

/* Deliberately not static, so other .c files of the library can reach it,
 * and chttpserver.c does. Such a caller must make sure this module's
 * merged _ccol_at_fork() triple registers BEFORE the caller's own.
 * pthread_atfork runs prepare handlers in LIFO order, so at every later
 * fork() the CALLER's prepare handler runs FIRST, before this module's
 * _cthreadcomm_atfork_prepare, which can then never lock
 * ccol_event_loop_slot_table.rwlock or a live ccol_event_loop's locks ahead
 * of the caller.
 *
 * cthreadcomm.h doesn't declare it (internal/catfork.h does) and it isn't
 * public API; it's a narrow, deliberate escape hatch, and a caller must read
 * and satisfy this exact ordering requirement first. The call site in
 * chttpserver.c explains the reasoning, including the lock-order inversion this
 * ordering rules out, which ThreadSanitizer reports when the two handler sets
 * may register in either order.
 *
 * A caller that never uses it isn't affected at all: this module's lazy,
 * _ccol_call_once-guarded registration happens on first use of a
 * ccol_event_loop, a ccol_circular_queue or a ccol_dynamic_queue. */
void _cthreadcomm_ensure_atfork_registered_before_caller(void) {
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
}

struct _ccol_event_loop_s {
  int epfd;
  int shutdown_efd;

  /* Registered in epfd just like shutdown_efd, but this fd gets drained and
   * can be re-armed (the per-event loop of _ccol_event_loop_thread_fn does
   * the drain), while shutdown_efd is one-shot by design: nothing ever
   * drains it or looks at it again. This one is for pinging the poller any
   * time poller_thread needs a prompt wake, not just once.
   *
   * ccol_event_loop_remove pings it whenever the removed reg has a non-NULL
   * on_removed handler, so that registration's callback fires with a small
   * and really bounded latency, at poller_thread's next between-batches
   * point, instead of only when some unrelated fd happens to become ready.
   * Picture a registration removed on an otherwise idle loop, nothing else
   * registered and nothing about to fire. Without the ping, the on_removed
   * notification and the memory reclaim that triggers it can both wait for
   * as long as epoll_wait's -1 (infinite) timeout keeps the poller blocked,
   * which can be until the loop is destroyed.
   *
   * That's more than a theoretical latency worry: without the ping, this
   * module's on_removed_fires_for_an_ordinary_removal test hangs under
   * ThreadSanitizer. There's a scheduling race between the thread creation
   * and the epoll_wait call that a fast, unloaded run gets through without
   * noticing, and ThreadSanitizer's slowdown widens it into a reliable
   * timeout.
   *
   * The ping is only a latency optimization and not what makes things
   * correct: the bounded epoll_wait retry of EVENT_LOOP_RECLAIM_RETRY_MS is
   * what guarantees a pending reclaim gets retried (see
   * _ccol_event_loop_reclaim_pending_frees and _ccol_event_loop_thread_fn). A
   * ping from _ccol_event_reg_resolve_unpin can be based on a stale snapshot
   * of reg->removed racing a concurrent ccol_event_loop_remove; see that
   * function's comment. */
  int reclaim_wake_efd;

  /* In every configuration exactly one dedicated thread calls epoll_wait on
   * epfd, which avoids the thundering-herd cost several threads sharing one
   * epoll instance would pay; the comment of dispatch_pool describes where
   * the throughput comes from instead. With num_reactor_threads == 1
   * dispatch_pool is never created, and this one thread polls, runs every
   * callback inline and does the reclamation too (see the epoch scheme
   * below). */
  _ccol_thread_id_t poller_thread;
  size_t num_reactor_threads; /* The caller-facing parameter. The total OS
                               * thread count for this loop is always exactly
                               * this value: 1 for poller_thread and, when
                               * the value is above 1,
                               * (num_reactor_threads - 1) worker threads that
                               * the ctpool owns. */

  /* NULL when num_reactor_threads == 1, otherwise num_reactor_threads - 1
   * worker threads that run the dispatch callbacks. poller_thread only
   * collects readiness and hands a heap-allocated job to this pool with
   * ctpool_submit. The pool's queue is unbounded, so a submission never
   * blocks the poller, and poller_thread never runs a callback itself. We
   * reuse cthreadpool.c's ctpool, which has its own tests, instead of
   * building a second worker pool and queue in this module, and since
   * cthreadpool.c doesn't depend on this header there's no risk of a
   * circular dependency the other way round. */
  ctpool dispatch_pool;

  /* Guards only shutdown_started, joined and joined_cv, which carry
   * ccol_event_loop_shutdown's one-shot leader and follower coordination. It
   * never touches per-fd state, so don't mistake it for a per-stripe lock;
   * that's exactly why it isn't called "registry_lock". The registry itself
   * lives in stripes[], each stripe with its own lock, and this lock guards
   * none of them. */
  _ccol_mutex_t shutdown_lock;
  _ccol_cond_var_t joined_cv;
  bool shutdown_started;
  bool joined;
  /* True once a __ccol_event_loop_destroy call owns this loop's teardown.
   * That call sets it while the handle still resolves, so the callbacks its
   * drain runs can still call back into this loop, and a second destroy call
   * reads it and fails fatally. Both the read and the write happen under the
   * write side of ccol_event_loop_slot_table.rwlock. */
  bool destroy_claimed;
  _Atomic bool shutting_down;

  /* event_entry structs that ccol_event_loop_remove retired and that aren't
   * freed yet (the comment of _ccol_event_loop_defer_entry_free explains why
   * freeing them synchronously there is a use-after-free). This is the head
   * of a lock-free Treiber stack, pushed with a CAS and drained with one
   * atomic_exchange. It's loop-wide rather than per stripe, since entries
   * from every stripe go onto this one list. */
  _Atomic(event_entry *) pending_entry_frees;

  /* ccol_event_reg structs whose refcount reached 0 and that aren't freed
   * yet. Usually the refcount reaches 0 immediately with no dispatch in
   * flight, and freeing synchronously there would leave a caller holding a
   * ccol_event_reg* to freed memory that it may still pass to
   * ccol_event_loop_modify or ccol_event_loop_remove. Both promise a graceful
   * ccol_invalid_args for an already removed reg, not undefined behaviour;
   * see the comment of _ccol_event_loop_defer_reg_free. Lock-free, with the
   * same shape as pending_entry_frees. */
  _Atomic(ccol_event_reg_s *) pending_reg_frees;

  /* Per-loop generation-tagged slot table for the PUBLIC ccol_event_reg
   * handle, a uint64_t with the slot index in its top 32 bits and the
   * generation in its bottom 32 bits. The design copies the process-wide
   * ccol_event_loop_slot_table exactly but covers one loop instead of the
   * whole process: a reg belongs to one loop the caller supplied, so it needs
   * no process-wide table, while a ccol_event_loop handle does, having no
   * owning object to scope it to.
   *
   * The table exists because a raw ccol_event_reg* in the caller's hands says
   * nothing about liveness. The caller can't tell "live" from "already freed"
   * without dereferencing it, and the dereference itself is unsafe once the
   * object may be gone. ccol_event_loop_modify, _pause, _resume, _remove and
   * ccol_event_loop_reg_generation all need at least a reg's stripe_idx
   * before any lock-protected liveness check can run.
   *
   * The epoch scheme below (poller_batch_gen) only proves that no STALE
   * POLLER BATCH can reference a reg when it is freed. It says nothing about a
   * second application thread dereferencing that same reg* through one of
   * those five entry points, concurrently or later, on any schedule.
   * Resolving the public handle through this table closes that gap completely
   * and unconditionally (see _ccol_event_reg_resolve and
   * _ccol_event_reg_resolve_unpin), just as resolving a ccol_event_loop's
   * handle through ccol_event_loop_slot_table protects a loop*. A stale or
   * already removed handle always fails the index and generation check that
   * runs under reg_slot_rwlock, so we never touch memory that may already be
   * freed. ccol_event_loop_remove marks a reg's slot not in use the moment it
   * removes the reg and bumps the slot's generation, so every later resolve of
   * that handle value fails at once whether or not the ccol_event_reg_s behind
   * it has been freed yet. The comment of
   * _ccol_event_loop_reclaim_pending_frees says when that free happens.
   *
   * The lock is a read-write lock, not a plain mutex. _ccol_event_reg_resolve
   * only reads (it bounds-checks the index, compares the generation and reads
   * slot->ptr) and it runs on every ccol_event_loop_pause, _resume and
   * _modify call. A caller like chttpserver pauses and resumes a
   * connection's registration once per request, so those calls run at the
   * full request rate, while the only mutators, _ccol_event_reg_slot_acquire
   * and _ccol_event_reg_slot_release, run once per registration lifetime and
   * are rare. A plain mutex would serialise every concurrent resolve of every
   * connection in the process behind one lock, while _ccol_rw_lock_rdlock
   * lets concurrent resolves run in parallel and excludes only the rare
   * acquire and release mutations, which may reallocate reg_slots' backing
   * storage. There is one catch: a glibc rwlock's write lock records the
   * writer's thread ID, which changes in a fork() child, while this module's
   * plain mutexes record none. The doc comment of
   * _cthreadcomm_atfork_prepare describes that hazard and how we deal with
   * it. */
  _ccol_rw_lock_t reg_slot_rwlock;
  cvec reg_slots;        /* cvec of ccol_event_reg_slot_t; grows via push_back
                             only, indices permanent once allocated */
  cvec reg_free_indices; /* cvec of uint32_t; LIFO free list, O(1) reuse */
  /* How many slot indices couldn't be pushed back onto reg_free_indices
     because the push couldn't allocate. The index is still released (its
     slot has ptr == NULL and in_use == false), but the free list can't reach
     it, so the acquire path scans for such a slot instead of growing
     reg_slots again. Guarded by reg_slot_rwlock like the two vectors above. */
  size_t reg_lost_indices;

  /* The deferred-free reclamation counter (the big comment above
   * _ccol_event_loop_reclaim_pending_frees gives the details). In every
   * configuration exactly one thread, poller_thread, calls epoll_wait, so
   * "safe to free" needs a single monotonic scalar rather than a per-thread
   * array. poller_thread bumps poller_batch_gen once at its between-batches
   * point, right before the next epoll_wait call. Every deferred item has its
   * own defer_gen (see event_entry and ccol_event_reg), a snapshot of
   * poller_batch_gen taken when it was deferred, and its EPOCH condition holds
   * once defer_gen < poller_batch_gen: poller_thread has crossed a
   * between-batches point since the deferral and so has completely finished
   * any batch it had in flight.
   *
   * For an event_entry that's only half the condition; the comment of
   * event_entry.refcount covers the other half, needed because a ctpool
   * worker's job can use an entry well after poller_thread has moved on (only
   * with num_reactor_threads > 1). A ccol_event_reg has no second condition,
   * because its own refcount fully covers an in-flight callback that needs
   * the reg, whether that callback runs inline on poller_thread or on a
   * dispatch_pool worker.
   *
   * The counter is 64 bits wide on every target. A 32-bit one wraps after
   * 2^32 batches, which a busy loop gets to in days, and an item deferred
   * just before the wrap would then carry a defer_gen above every later
   * value, never become eligible, and keep the poller on its bounded retry
   * timeout for the rest of the loop's life. */
  _Atomic uint64_t poller_batch_gen;

  /* Mints the caller-visible identity token of
   * ccol_event_loop_reg_generation. Loop-wide, monotonic, starting at 1, with
   * 0 reserved for "no reg" (see the doc comment of
   * ccol_event_loop_reg_generation). It goes up once per NEW event_entry, not
   * once per ccol_event_loop_add call: a second direction joining an already
   * registered fd shares the existing entry's generation instead of minting
   * a new one. */
  _Atomic uint64_t fd_generation_counter;

  size_t max_events_per_wait;

  /* poller_thread's epoll_wait buffer, max_events_per_wait entries.
   * ccol_event_loop_create_mp allocates it before starting that thread and
   * _ccol_event_loop_teardown_raw frees it after the join. The thread itself
   * allocates nothing, so a loop whose create returned a handle always has a
   * poller that dispatches. */
  ccol_poll_event *poller_events;

  /* Lock-striped fd/entry registry: num_stripes independent (mutex, chmap,
   * queue-reg list) triples. See ccol_event_loop_stripe_t and
   * _stripe_index_for_fd. */
  ccol_event_loop_stripe_t *stripes;
  size_t num_stripes;

  /* Round-robin cursor that assigns a queue or ccol_channel registration to a
   * stripe (see ccol_event_loop_add). Unlike a real fd, a queue selectable's
   * entry is never looked up by a second call, since two of them are never
   * combined, so the assignment has no consistency rule to follow and any
   * rule that is deterministic per registration works. Round-robin is simpler
   * than hashing bridge_efd, an fd that doesn't even exist yet when the
   * stripe is chosen (it's created only after the stripe lock is taken), and
   * it spreads registrations strictly better too. */
  _Atomic size_t next_queue_stripe;

  _Atomic size_t reg_count;

  ccol_memmgmt_procs_t *m_procs;

  /* _ccol_event_loop_resolve pins this count with a lock-free atomic
   * increment, and the pin stays while a caller holds a resolved struct
   * _ccol_event_loop_s* it hasn't released with
   * _ccol_event_loop_resolve_unpin yet. Unlike the field of the same name in
   * chttpcli and chttpsvr, the unpin here is ALSO a bare atomic decrement and
   * not a lock-protected one. ccol_event_loop is lock-striped precisely to
   * keep every hot per-registration call (ccol_event_loop_add, _modify,
   * _pause, _resume and _remove) free of a single global lock, and a
   * lock-protected unpin would put exactly that lock back on all of them. So
   * __ccol_event_loop_destroy polls until this count reaches 0 (see its own
   * comment). A poll never depends on a signal being delivered, so unlike a
   * condition variable wait it can't lose a wakeup. */
  _Atomic size_t pending_resolve_count;

  /* This loop's public handle value, minted once by
   * _ccol_event_loop_handle_slot_acquire and never changed after. The
   * callbacks need it: ccol_event_readable_fn, ccol_event_writable_fn and
   * ccol_event_error_fn all take the public ccol_event_loop handle as their
   * `loop` argument, never the raw struct _ccol_event_loop_s* this file uses
   * internally, so application code in a callback that calls back into
   * ccol_event_loop_modify, _pause, _resume, _add or _remove goes through the
   * ordinary resolve and pin steps like any other caller (see the two call
   * sites in _ccol_event_loop_run_callback). It's a plain field and needs no
   * synchronization: it's written exactly once before the loop's constructor
   * returns the handle, and dispatch can only start after the caller has the
   * handle, since nothing can be registered before then, so no callback can
   * read this field before it holds its final value. */
  ccol_event_loop self_handle;

  /* Only this process's CHILD-side fork handler,
   * _ccol_event_loop_atfork_child_release, sets this to true, for every
   * ccol_event_loop instance marked in_use when fork() happened. fork()
   * copies only the calling thread, so as far as this process is concerned
   * poller_thread is just inert copy-on-write memory: it never had an
   * execution context here and never will.
   *
   * Once this is true, _ccol_event_loop_shutdown_internal must never do two
   * things, and treats both as already done. It must not call
   * _ccol_thread_join on poller_thread, which is undefined behaviour because
   * this process never created that pthread_t and can never join it. And it
   * must not write to shutdown_efd, a real kernel object that fork() shares
   * instead of copying, so a write would wrongly wake the PARENT's poller
   * thread, which really does use that object.
   *
   * dispatch_pool needs no such check here. It's a ctpool, and cthreadpool.c
   * has its own identical foreign_since_fork field that makes an
   * unconditional ctpool_shutdown_drain call safe whatever this flag says.
   * Without that field on the ctpool side, forking a process that holds a
   * live ccol_event_loop with num_reactor_threads > 1 and then calling
   * ccol_event_loop_destroy on the inherited handle in the child crashes with
   * SIGSEGV inside glibc's __pthread_clockjoin_ex, which
   * ctpool_shutdown_drain's worker-thread join loop reaches.
   *
   * It's never true for a loop this process really created with
   * ccol_event_loop_create_mp. With CCOL_FORK_SAFETY_REQUIRED at 0 the
   * compiler removes the flag entirely (see that macro's doc comment in
   * common.h), and every site that would read it takes the same path it
   * takes when the field is false. */
#if CCOL_FORK_SAFETY_REQUIRED
  _Atomic bool foreign_since_fork;
#endif

#ifdef RUNNING_UNIT_TESTS
  /* Test-only. Bumped once per epoll_wait call that completes on
   * poller_thread, however many events it returned, zero included. A test
   * can then spot a busy spin directly, because during one this counter
   * races far ahead within a short, bounded sampling window, with no flaky
   * wall-clock or CPU measurement (see
   * _ccol_event_loop_poller_iterations_for_tests). It stays behind
   * RUNNING_UNIT_TESTS so a production build pays nothing for it, in line
   * with the project's performance-first policy. */
  _Atomic uint64_t poller_iterations_for_tests;
  /* Test-only. How many event entries
   * _ccol_event_loop_reclaim_pending_frees's epoch reclaim has freed. A test
   * reads it to know that a removed registration's entry is really gone
   * before it checks a later registration of the same fd. See
   * _ccol_event_loop_entries_reclaimed_for_tests. */
  _Atomic uint64_t entries_reclaimed_for_tests;
#endif
};

/* ========================================================================== */
/*                         FORK SAFETY (pthread_atfork)                       */
/* ========================================================================== */

#if CCOL_FORK_SAFETY_REQUIRED
/* fork() copies only the calling thread, but some OTHER thread may hold a
 * lock at that instant, and the child then inherits that lock locked for
 * good, since no surviving thread in the child can unlock it. So this
 * function locks every lock this module can plausibly be holding at an
 * arbitrary moment, in three groups: the process-wide
 * ccol_event_loop_slot_table.rwlock; each live loop's shutdown_lock,
 * reg_slot_rwlock and stripes[].lock; and the mutex of every live
 * ccol_circular_queue and ccol_dynamic_queue. Every ccol_event_loop_create,
 * _destroy, _add, _remove, _modify, _pause and _resume call takes the first
 * one, through _ccol_event_loop_resolve or
 * _ccol_event_loop_handle_slot_acquire. The queue mutexes come from
 * queue_mutex_registry (see that registry's doc comment above).
 *
 * prepare() takes all of it before fork() can proceed, so fork() completes
 * only at a moment when no thread holds any of them, and parent() and
 * child() release it all again through one shared function. Every PLAIN
 * mutex in this module is the default "normal" pthread type, which on Linux
 * glibc tracks no owner or TID, so a plain pthread_mutex_unlock is well
 * defined even from a thread that didn't lock it; and for anything the
 * forking thread didn't hold itself, the original holder doesn't even exist
 * in the child.
 *
 * ccol_event_loop_slot_table.rwlock and reg_slot_rwlock are the two
 * exceptions, and both need a reinit in the child. prepare() takes the write
 * side of both on the forking thread once every other holder has let go,
 * ccol_event_loop_slot_table.rwlock at its outer scope and reg_slot_rwlock
 * inside Phase 1's per-loop walk. A glibc rwlock's unlock tells a writer
 * unlock from a reader unlock by comparing the writer TID it recorded with
 * the caller's TID, and fork() gives the child's single thread a new TID, so
 * a plain _ccol_rw_lock_unlock in the child returns 0, releases nothing, and
 * every later lock of it there blocks for ever. clogger.c's atfork handling
 * of clog_slot_table.rwlock, which this mirrors, deals with the same case. A
 * plain mutex keeps no TID, which is why only these two rwlocks, out of all
 * the locks here, need the reinit.
 *
 * Without this locking the hang is real and reproducible. Take a thread that
 * keeps creating and destroying unrelated ccol_event_loop instances and race
 * it against repeated fork() calls: about 1 forked child in 1000 hangs for
 * good the moment it makes its own brand-new ccol_event_loop_create_mp call,
 * because it inherited ccol_event_loop_slot_table's lock locked. Or take one
 * constantly busy multi-threaded reactor, like the long-lived process-wide
 * reactors of chttpserver.c and chttpclient.c, and fork it while it's busy:
 * about half of the children hang as soon as they try one more
 * ccol_event_loop_add on the inherited loop, because a stripe lock arrives
 * locked, namely the stripe holding the fd that the vanished poller and
 * dispatch threads were working on.
 *
 * This deliberately does NOT extend to individual event_entry.dispatch_lock
 * locks. The only safe way to find every live entry is to walk a stripe's
 * fd_index and queue_regs_head under that very stripe's lock, and locking a
 * dispatch lock while holding the stripe lock is the exact reverse of the
 * one ordering between an entry's dispatch lock and a stripe lock that this
 * file uses (see the field comment of event_entry.dispatch_lock): we always
 * take dispatch_lock first and release it before even considering a stripe
 * lock, never the other way round. Locking it here would add a real new ABBA
 * deadlock, the other side being an ordinary reactor thread in
 * _ccol_event_loop_handle_event that holds dispatch_lock and wants a stripe
 * lock, and it would fix nothing.
 *
 * Since fork() copies only the calling thread, a forked child never has a
 * reactor or dispatch thread of its own for a loop it merely inherited, so
 * nothing in the child can ever legitimately dispatch through that loop
 * again, gap or no gap. The one thing the gap leaves unprotected is a later
 * ccol_event_loop_destroy() of that inherited loop in the child racing a
 * dispatch_lock that some parent-side thread, gone by then, held at fork
 * time. POSIX already makes _ccol_mutex_destroy on a locked mutex undefined
 * behaviour, fork or no fork. This case is narrower and far less likely than
 * the two hazards above (roughly 1-in-1000 and 1-in-2), so we document it
 * here instead of closing it. Closing it safely needs another way to find
 * live entries, such as a dedicated live-entry list with its own lock like
 * clogger.c's live_shareds, which is a much larger change than this
 * mechanism's scope.
 *
 * Unlike dispatch_lock, this DOES extend to the wait_mtx of every
 * ccol_event_reg_s backed by a queue or a ccol_channel, and to the cq->mutex
 * or dq->mutex of the queue behind that reg. The two must be locked in
 * exactly this relative order, which matches every real nested-locking
 * pattern in this file:
 *
 *   1. Every live loop's shutdown_lock and reg_slot_rwlock (write side), and
 *      every stripe's lock. This matches the stripe->lock before cq->mutex
 *      nesting of _ccol_event_loop_add_queue and
 *      _ccol_event_loop_remove_unlink, which callers enter with the owning
 *      stripe's lock already held, so that lock must be held here before
 *      we touch the queue mutex behind any of the stripe's queue-backed
 *      registrations.
 *   2. The mutex of every live queue, from queue_mutex_registry. Each one is
 *      locked exactly once however many ccol_event_loop registrations refer
 *      to it, across one or more stripes and loops, or none: the registry
 *      never lists an address twice, so a single pass over it as its own
 *      phase can never double-lock anything.
 *   3. The wait_mtx of every queue-backed registration. This matches the
 *      cq->mutex before wait_mtx nesting of _notify_waiter, which any
 *      ordinary producer or consumer thread, completely independent of this
 *      loop's reactor and dispatch threads, calls from a plain send or
 *      receive on a queue this registration watches, always while holding
 *      that queue's mutex.
 *
 * A SEPARATE, independent _ccol_at_fork() registration for the queue mutexes
 * is a real deadlock. Create a ccol_circular_queue before the process's
 * first ccol_event_loop, which registers the separate _ccol_at_fork() triple
 * first. pthread_atfork runs prepare handlers in REVERSE registration order,
 * so ccol_event_loop's prepare runs FIRST and locks a queue-backed
 * registration's wait_mtx, and the queue registry's separate prepare runs
 * SECOND and tries to lock the cq->mutex of that SAME queue. A truly
 * concurrent ccol_circq_send_zc holds that mutex while it is itself blocked
 * in _notify_waiter wanting that same wait_mtx: a textbook AB-BA cycle, with
 * the forking thread deadlocked inside fork() itself.
 *
 * The three-phase design closes this completely by construction. There is
 * exactly ONE _ccol_at_fork() registration, so nothing depends on the
 * accident of "which of two independent handler sets runs first", and its
 * three phases satisfy constraints 1 and 3 above at the same time for every
 * queue-backed registration, whatever stripe and loop it belongs to, and
 * also when its queue has no ccol_event_loop registration at all.
 *
 * This only ever walks slots with in_use == true, mirroring exactly the
 * condition _ccol_event_loop_resolve trusts as the only sign that
 * slot->ptr is safe to dereference. __ccol_event_loop_destroy keeps in_use
 * set while its graceful shutdown joins the poller thread and drains
 * dispatch_pool, so the walk covers a loop in that phase too. It clears
 * in_use under the write side of this same ccol_event_loop_slot_table.rwlock
 * once that shutdown returns, and only then frees every entry, every
 * registration and the loop struct. By then no thread of the loop is
 * running, so the locks this walk skips have no holder left except the
 * destroying thread itself. */
static void _cthreadcomm_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(_ccol_atfork_module_cthreadcomm);
#endif
  _ccol_mutex_lock(queue_mutex_registry.mutex);
  _ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);

  size_t n_loops = _event_loop_slot_count_locked();

  /* Phase 1: every live loop's shutdown_lock, reg_slot_rwlock and stripe
   * locks. This phase deliberately leaves wait_mtx and every queue mutex
   * alone for now; the function's doc comment explains why those have to
   * wait for phases 2 and 3.
   *
   * We take reg_slot_rwlock's WRITE side here, not its read side, because
   * fork() must see this table fully quiet, with no resolve, acquire or
   * release in flight anywhere, and only the write side guarantees that
   * against every other locker, readers included. That field's comment
   * explains why it's a rwlock at all, and the comment of
   * _cthreadcomm_atfork_release_impl the child-side release hazard that
   * follows, which applies to this lock and to no other one taken here. */
  for (size_t i = 0; i < n_loops; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct _ccol_event_loop_s *loop = slot->ptr;
    _ccol_mutex_lock(loop->shutdown_lock);
    _ccol_rw_lock_wrlock(loop->reg_slot_rwlock);
    for (size_t s = 0; s < loop->num_stripes; s++) {
      _ccol_mutex_lock(loop->stripes[s].lock);
    }
  }

  /* Phase 2: each live queue's own mutex, exactly once, now that every
   * stripe lock (phase 1) is held. */
  size_t n_addrs = queue_mutex_registry.addrs
                       ? cvector_elem_count(queue_mutex_registry.addrs)
                       : 0;
  for (size_t k = 0; k < n_addrs; k++) {
    _ccol_mutex_t *m = ((_queue_mutex_registry_entry *)cvector_at(
                            queue_mutex_registry.addrs, k))
                           ->mutex;
    _ccol_mutex_lock(*m);
  }

  /* Phase 3: each queue-backed registration's own wait_mtx, now that every
   * queue's mutex (phase 2) is held. */
  for (size_t i = 0; i < n_loops; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct _ccol_event_loop_s *loop = slot->ptr;
    for (size_t s = 0; s < loop->num_stripes; s++) {
      for (ccol_event_reg_s *reg = loop->stripes[s].queue_regs_head; reg;
           reg = reg->loop_list_next) {
        _ccol_mutex_lock(reg->wait_mtx);
      }
    }
  }
}

/* Shared by parent() and child(). The doc comment of
 * _cthreadcomm_atfork_prepare explains why a plain unlock rather than a
 * reinit is right in both branches for this module's mutexes, and describes
 * the three-phase design mirrored here. We run the phases in reverse order,
 * which makes no difference to correctness with this module's plain,
 * non-recursive mutexes, since an unlock never blocks and can't deadlock.
 *
 * Walking exactly the structure prepare() just walked and releasing every
 * lock is safe, because nothing can have changed the slot table, the queue
 * registry, a live loop's stripe count or its registration list in between:
 * every lock such a change would need is held right now.
 *
 * With is_child we do more for every live loop:
 *  - Mark it foreign_since_fork (see that field's comment), so a later
 *    ccol_event_loop_shutdown or _destroy call in THIS process never joins
 *    poller_thread and never signals shutdown_efd.
 *  - Reset its pending_resolve_count to 0, for the same reason as the
 *    is_child branch of _ctpool_atfork_release_impl: as far as this process
 *    can tell, a vanished parent-side thread may have left the count stuck
 *    above zero. The full reasoning in that function's comment applies here
 *    as is.
 *  - Replace this process's local epfd with a brand new, empty epoll
 *    instance. loop->epfd is a real kernel object, and so is
 *    loop->shutdown_efd's registration inside it, and fork() duplicates the
 *    reference instead of deep-copying the object, so this process's fd
 *    number names the SAME epoll instance that the parent's still running
 *    poller thread keeps calling epoll_wait on.
 *
 *    Without the swap, every later epoll_ctl call of this process (from
 *    ccol_event_loop_add, _remove, _modify, _pause and _resume) silently
 *    edits the PARENT's live interest set, even though they are otherwise
 *    perfectly working bookkeeping operations on this loop's private
 *    copy-on-write registry. An ADD for a really new fd registered here can
 *    hand the parent's poller an epoll_event whose ev.data.ptr is an
 *    event_entry* that only makes sense in this process's diverged heap, and
 *    a DEL for an inherited registration pulls the parent's interest in that
 *    fd, which the parent wants to keep, out from under it.
 *
 *    Swapping the epfd decouples every later epoll_ctl of this process from
 *    the parent's kernel object, and it costs this process nothing in
 *    correctness: no poller thread of its own survives the fork, so it was
 *    never going to epoll_wait on that instance for a real dispatch (see the
 *    doc comment of ccol_event_loop_add: nothing in the child can ever
 *    legitimately dispatch through that loop again). A later
 *    ccol_event_loop_remove or ccol_event_loop_destroy of an inherited,
 *    pre-fork registration then finds nothing to EPOLL_CTL_DEL on the new
 *    empty instance, which is silently fine since this whole file ignores the
 *    return value of every such epoll_ctl call. And that's the point: it must
 *    not find, or touch, the parent's live registration for that fd.
 *
 *    A failure to create the replacement is silently tolerated too. In that
 *    rare case loop->epfd stays the original, shared one instead of aborting
 *    a fork() that otherwise worked, and nothing else in this function
 *    depends on the swap succeeding. */
static void _cthreadcomm_atfork_release_impl(bool is_child) {
  size_t n_loops = _event_loop_slot_count_locked();

  for (size_t i = 0; i < n_loops; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct _ccol_event_loop_s *loop = slot->ptr;

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
        _ccol_mutex_unlock(reg->wait_mtx);
      }
    }
  }

  /* Each live queue's mutex, exactly once. They're unlocked in this one
   * separate pass, mirroring prepare()'s phase 2 exactly, rather than inside
   * the loop above, because registrations on several stripes or loops can
   * share one queue's mutex and unlocking it more than once is undefined
   * behaviour for this module's plain mutexes. */
  size_t n_addrs = queue_mutex_registry.addrs
                       ? cvector_elem_count(queue_mutex_registry.addrs)
                       : 0;
  for (size_t k = 0; k < n_addrs; k++) {
    _ccol_mutex_t *m = ((_queue_mutex_registry_entry *)cvector_at(
                            queue_mutex_registry.addrs, k))
                           ->mutex;
    _ccol_mutex_unlock(*m);
  }

  for (size_t i = 0; i < n_loops; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct _ccol_event_loop_s *loop = slot->ptr;
    for (size_t s = 0; s < loop->num_stripes; s++) {
      _ccol_mutex_unlock(loop->stripes[s].lock);
    }
    /* Phase 1 above took reg_slot_rwlock's write side on this same thread,
     * the one calling fork(), since pthread_atfork runs prepare(), parent()
     * and child() all on it. In the parent a plain _ccol_rw_lock_unlock from
     * HERE releases it: a glibc rwlock's unlock releases the write side only
     * when the caller's TID matches the writer TID it recorded, and here it
     * does.
     *
     * In the child it doesn't, because fork() gives the child's single
     * thread a new TID, so a plain unlock returns 0, releases nothing, and
     * every later resolve in the child blocks for ever. So the child
     * reinitializes the rwlock instead, as _clog_atfork_release in clogger.c
     * does for clog_slot_table.rwlock. That's safe because the child has
     * exactly one thread at this point, so nothing can be waiting on the
     * lock. */
    if (is_child) {
      if (_ccol_rw_lock_reinit_in_child(loop->reg_slot_rwlock) != 0)
        _ccol_fatal_err(
            "ccol_event_loop atfork release: failed to reinit reg_slot_rwlock");
    } else {
      _ccol_rw_lock_unlock(loop->reg_slot_rwlock);
    }
    _ccol_mutex_unlock(loop->shutdown_lock);
  }

  /* prepare() took ccol_event_loop_slot_table.rwlock's write side on this
   * same thread; the identical in_child and else treatment of
   * reg_slot_rwlock just above explains why the child reinitializes it.
   * src/cthreadpool.c has its own copy of exactly this treatment in
   * _ctpool_atfork_release_impl, and this is the same hazard for the
   * process-wide loop table, not a new one. */
  if (is_child) {
    if (_ccol_rw_lock_reinit_in_child(ccol_event_loop_slot_table.rwlock) != 0)
      _ccol_fatal_err(
          "ccol_event_loop atfork release: failed to reinit "
          "ccol_event_loop_slot_table "
          "rwlock");
  } else {
    _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  }
  _ccol_mutex_unlock(queue_mutex_registry.mutex);
}

static void _cthreadcomm_atfork_release(void) {
  _cthreadcomm_atfork_release_impl(false);
}

/* Child-side counterpart of _cthreadcomm_atfork_release. The comment of
 * _cthreadcomm_atfork_release_impl describes the extra child-only work and
 * why each part of it is needed. It must run before any application code
 * in this process can reach one of these loops' shutdown, destroy, add,
 * remove or modify path, and it does: pthread_atfork's child handler runs
 * synchronously as part of fork()'s own return, strictly before fork()'s
 * return value reaches the calling code. */
static void _cthreadcomm_atfork_child_release(void) {
  _cthreadcomm_atfork_release_impl(true);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                    EVENT_LOOP HANDLE RESOLVE / UNPIN                       */
/* ========================================================================== */

/* Resolves h and pins the result against a concurrent destroy. Returns NULL
 * for an h of 0, for garbage, and for a slot that is free right now or has
 * been reused by a later acquire (so the generation is wrong). On success the
 * caller MUST call _ccol_event_loop_resolve_unpin(result) exactly once, as
 * soon as it stops touching the resolved struct _ccol_event_loop_s*. */
static struct _ccol_event_loop_s *_ccol_event_loop_resolve(ccol_event_loop h) {
  _ccol_call_once(ccol_event_loop_slot_table.once,
                  _ccol_event_loop_slot_table_init_globals);
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  _ccol_rw_lock_rdlock(ccol_event_loop_slot_table.rwlock);
  struct _ccol_event_loop_s *raw = NULL;
  if (idx < _event_loop_slot_count_locked()) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* No lock on raw itself here, in line with the field comment on
   * pending_resolve_count: ccol_event_loop's lock striping exists to keep
   * every hot per-registration call free of a single global lock, and this
   * resolve step mustn't add one. It's safe because raw is certainly still
   * allocated at this point. Only __ccol_event_loop_destroy's slot-release
   * step can make raw unsafe to touch, and that step needs the write side of
   * ccol_event_loop_slot_table.rwlock, so it can never overlap this read
   * side. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  return raw;
}

static void _ccol_event_loop_resolve_unpin(struct _ccol_event_loop_s *raw) {
  /* A bare atomic decrement with no lock and no broadcast, where chttpcli and
   * chttpsvr use a lock-protected one; the field comment of
   * pending_resolve_count explains why that difference is right here.
   * __ccol_event_loop_destroy polls for this count to reach 0 instead of
   * sleeping on a condition variable, so there's no lost wakeup to guard
   * against and nobody to broadcast to. */
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
}

/* Allocates a fresh slot for loop, or reuses a freed one, and returns the
 * resulting handle, or 0 when it can't allocate. ccol_event_loop_create_mp
 * calls it once, when the loop is otherwise fully built, including its
 * poller thread and, if configured, its dispatch_pool, so a failed slot
 * acquire here needs to stop those and not just free memory. That function's
 * comment explains why. */
#ifdef RUNNING_UNIT_TESTS
/* Test-only. Makes the next push onto the loop free list behave exactly like
 * an allocation failure, then disarms itself. Otherwise the lost-index
 * recovery below would need a real out-of-memory condition at one specific
 * cvector_push_back call, which an ordinary test run can't produce. */
static _Atomic bool g_fail_next_loop_free_index_push = false;

void _ccol_event_loop_force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_fail_next_loop_free_index_push, true);
}
#endif

/* Hands idx back for reuse; the caller holds the write lock.
 *
 * A failed push gets recorded instead of swallowed. The slot the index names
 * is already fully released, so silently dropping it would strand that index
 * for the life of the process and make every later create grow the table
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

/* Recovers one slot index stranded by a failed free-list push (see
 * ccol_event_loop_slot_table.lost_indices) by scanning for a released slot
 * that nothing names. The caller holds the write lock and has found
 * free_indices empty, which is what makes "ptr is NULL and in_use is false"
 * mean lost and not just free, since every successfully released slot is on
 * that list. A slot whose destroy is in progress keeps a non-NULL ptr until
 * the very end of the destroy (in_use is cleared once its shutdown has
 * drained, ptr only in the final locked step), so the two conditions together
 * skip exactly that window. It only ever returns an index the table already
 * holds, so it can never push the table past any slot ceiling.
 *
 * It's out of line and only reached when the counter is above zero, so an
 * ordinary acquire pays a single comparison for it. When the scan finds
 * nothing it zeroes the counter, so a counter that somehow outlives its slot
 * can't make every later acquire scan the table. */
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
    struct _ccol_event_loop_s *loop) {
  _ccol_call_once(ccol_event_loop_slot_table.once,
                  _ccol_event_loop_slot_table_init_globals);
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  _ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  /* The process-exit destructor has released the table, so no handle can be
   * minted any more and the create fails as if an allocation had failed. */
  if (!ccol_event_loop_slot_table.slots) {
    _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
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
      _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(ccol_event_loop_slot_table.slots) - 1;
    slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that would collide with the reserved
   * "invalid handle" sentinel 0, which happens after about 2^32 reuses of
   * this exact slot index. The identical guard in
   * chttpcli_handle_slot_acquire explains why. */
  if (slot->generation == 0) slot->generation++;
  slot->ptr = loop;
  slot->in_use = true;
  ccol_event_loop h =
      ((ccol_event_loop)idx << 32) | (ccol_event_loop)slot->generation;
  _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  return h;
}

/* Knuth's multiplicative hash, reduced to [0, num_stripes) from its HIGH
 * bits: the 32-bit product times num_stripes, shifted down by 32. The kernel
 * picks an fd, not a peer, so an unkeyed hash is enough. Bit k of the
 * product depends only on bits 0 to k of the fd, so reading the low bits (a
 * modulo by a power of two) would cluster exactly like a plain
 * fd % num_stripes, with half of the stripes getting only even fds, while
 * the high bits depend on every bit of the fd. A stripe count above
 * UINT32_MAX, which no machine can allocate, takes the hash itself, which is
 * below it, so the product always fits in 64 bits. This is for fd
 * selectables only; queue and ccol_channel selectables get their stripe from
 * loop->next_queue_stripe (see ccol_event_loop_add), since their stripe has
 * no consistency rule to follow at all. */
static size_t _stripe_index(size_t num_stripes, int fd) {
  uint32_t h = (uint32_t)fd * 2654435761u;
#if SIZE_MAX > UINT32_MAX
  if (num_stripes > UINT32_MAX) return (size_t)h;
#endif
  return (size_t)(((uint64_t)h * (uint64_t)num_stripes) >> 32);
}

static size_t _stripe_index_for_fd(struct _ccol_event_loop_s *loop, int fd) {
  return _stripe_index(loop->num_stripes, fd);
}

#ifdef RUNNING_UNIT_TESTS
size_t _ccol_event_loop_test_stripe_index(size_t num_stripes, int fd) {
  return _stripe_index(num_stripes, fd);
}
#endif

/* fd_index maps a ccol_int key to a ccol_pointer value, both integral and at
 * most 8 bytes, so chashmap's should_use_open_addressing() puts the map on
 * the open-addressing backend instead of separate chaining. That backend's
 * oa_slot storage is _Alignas(max_align_t), so the memcpy below is defence in
 * depth and not a live alignment requirement. We keep it as a second line of
 * defence, consistent with the chmap-backed pointer storage of cjson, cyaml,
 * clrucache and chttpclient, which read their child pointers with memcpy the
 * same way, and because a caller of this map shouldn't count on one
 * particular backend for ever.
 *
 * All these functions work on ONE stripe's fd_index and queue_regs_head,
 * which the caller passes in directly after computing the right stripe (with
 * _stripe_index_for_fd or the round-robin counter) and locking it. None of
 * them works on the loop as a whole. */
static event_entry *_fd_registry_find(ccol_event_loop_stripe_t *stripe,
                                      int fd) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  cmap_pair val_pair;
  if (chmap_get_elem_ref(stripe->fd_index, &key_pair, &val_pair) !=
      ccol_success) {
    return NULL;
  }
  event_entry *entry;
  memcpy(&entry, val_pair.ptr, sizeof(entry));
  return entry;
}

/* Inserts fd->entry. The caller has already confirmed that fd isn't present,
 * through an earlier _fd_registry_find call that returned NULL. */
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

/* Finds the queue mutex for sel together with the right
 * sel_{read,write}_waiters_head and sel_{read,write}_rotor pointers. sel is
 * only ever a circq or a dynq: an fd selectable never gets here, and
 * ccol_selectable_from_chan has already turned a chan selectable into a
 * concrete circq before sel reaches ccol_event_loop_add. */
static void _queue_sel_locate(ccol_selectable *sel, _ccol_mutex_t **out_mtx,
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

/* These two give the queue a resolved queue selectable names and that
 * queue's dispatch-reference counter. sel is only ever a circq or a dynq: an
 * fd selectable never reaches either, and ccol_selectable_from_chan has
 * already turned a channel selectable into one of its two circular queues
 * by the time it gets here. */
static void *_queue_sel_queue(ccol_selectable *sel) {
  return (sel->type == ccol_selectable_circq) ? (void *)sel->cq
                                              : (void *)sel->dq;
}

static _Atomic size_t *_queue_sel_dispatch_refs(ccol_selectable *sel) {
  return (sel->type == ccol_selectable_circq) ? &sel->cq->dispatch_refs
                                              : &sel->dq->dispatch_refs;
}

/* The detached-registration count of the queue sel names. The caller holds
 * that queue's mutex. */
static size_t *_queue_sel_detached_count(ccol_selectable *sel) {
  return (sel->type == ccol_selectable_circq) ? &sel->cq->detached_loop_regs
                                              : &sel->dq->detached_loop_regs;
}

/* Claims a reference to the queue sel names, so that the queue's destroy
 * waits for this dispatch instead of freeing the queue from under it. The
 * caller holds the stripe lock that also guards the reg->removed check it
 * just passed, and ccol_event_loop_remove stores that flag under the same
 * lock. So a removal either comes first, and no reference is taken at all,
 * or comes second and finds this reference already published. Once remove
 * returns, no further reference can be claimed.
 *
 * Only a dispatch that is about to run its callback may claim it. With
 * num_reactor_threads == 1 it's claimed at collection time, since the
 * callback runs right after on the same thread, while the dispatch-pool path
 * claims it when the job starts on a worker and never when the poller
 * collects the job. If a queued job held a reference, a destroy could end up
 * waiting for a job stuck behind the very worker running that destroy (the
 * worker is running another registration's callback that tears this queue
 * down, which the documentation allows), and that wait would never end and
 * take the worker with it for good. A job that starts after the removal
 * finds reg->removed set, claims nothing and doesn't touch the queue at
 * all. */
static void _queue_dispatch_ref_acquire(ccol_selectable *sel) {
  atomic_fetch_add(_queue_sel_dispatch_refs(sel), (size_t)1);
}

/* How many messages the queue sel names holds. The caller holds that queue's
 * mutex. */
static size_t _queue_sel_msg_count(ccol_selectable *sel) {
  return (sel->type == ccol_selectable_circq) ? sel->cq->msg_count
                                              : sel->dq->msg_count;
}

/* Consumes a queue-backed registration's outstanding wake by clearing
 * reg->wait_ready and draining reg->bridge_efd in one critical section under
 * reg->wait_mtx. _notify_waiter sets the flag and writes the eventfd in one
 * critical section under the same mutex, so a notify either lands before
 * this call, with its message already in the queue for the callback that
 * follows, or lands after it, finds the flag clear and writes the eventfd
 * again, and the dispatch after the next epoll_wait covers it.
 *
 * When queue_count isn't NULL the queue is alive (reg isn't removed) and the
 * queue's message count also goes into *queue_count. It's read under the
 * queue mutex, and wait_mtx is taken before the queue mutex is released, so
 * every send or receive after the snapshot holds the queue mutex after this
 * call and its notify runs after the clear, leaving a wake pending. The
 * cascade step after the callback compares the count with this snapshot to
 * learn whether the dispatch moved any message (see
 * _ccol_event_loop_queue_cascade_notify_next). The queue mutex isn't held
 * across the drain, so producers never wait on that read(2). The hop count
 * of the consumed wake (see ccol_sel_waiter's fwd_hops field) goes into
 * *hops and is reset; the cascade step uses it to judge a dispatch that
 * moved nothing.
 *
 * The caller holds reg's stripe lock, and the order stripe lock, queue
 * mutex, wait_mtx agrees with every other path. */
static void _queue_reg_consume_wake(ccol_event_reg_s *reg, size_t *queue_count,
                                    uint32_t *hops) {
  if (queue_count) {
    _ccol_mutex_t *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
    _ccol_mutex_lock(*q_mtx);
    *queue_count = _queue_sel_msg_count(&reg->sel);
    _ccol_mutex_lock(reg->wait_mtx);
    _ccol_mutex_unlock(*q_mtx);
  } else {
    _ccol_mutex_lock(reg->wait_mtx);
  }
  reg->wait_ready = false;
  if (hops) *hops = reg->waiter_node.fwd_hops;
  reg->waiter_node.fwd_hops = 0;
  _eventfd_drain(reg->bridge_efd);
  _ccol_mutex_unlock(reg->wait_mtx);
}

/* Releases a reference _queue_dispatch_ref_acquire claimed. Nothing on this
 * path may touch the queue afterwards, because a destroy blocked on the
 * counter can carry on the instant this returns. */
static void _queue_dispatch_ref_release(ccol_selectable *sel) {
  atomic_fetch_sub(_queue_sel_dispatch_refs(sel), (size_t)1);
}

/* The per-loop slot table entry behind a public ccol_event_reg handle value.
 * The reg_slots field comment of struct _ccol_event_loop_s gives the
 * design.
 */
typedef struct {
  ccol_event_reg_s *ptr; /* NULL when the slot is free */
  uint32_t generation;   /* The code mints it fresh on every acquire. It rises
                             for each slot index. It is 0 before the first
                             use, and it becomes 1 on the first acquire. */
  bool in_use;
} ccol_event_reg_slot_t;

#ifdef RUNNING_UNIT_TESTS
/* Test-only. Makes the next _ccol_event_reg_slot_acquire call report a
 * failure, as if loop->reg_slots' growth allocation had failed, whatever
 * allocator the loop really uses, then disarms itself.
 *
 * A public constructor's allocator parameter can't reliably fail exactly
 * this one call. loop->reg_slots and reg_free_indices both get a non-zero
 * minimum capacity when the loop is created (cvector's "min capacity 4"
 * policy), so an ordinary ccol_event_loop_add only hits a real growing
 * reallocation once a loop holds several live registrations, and even then
 * the SAME per-loop allocator serves every other allocation of this module,
 * the fd registry's chmap and dispatch_pool included. Fault injection alone
 * can't fail this one call in isolation, so the hook follows this codebase's
 * usual approach for this kind of hard-to-reach allocation-failure test; see
 * _clog_test_force_next_fresh_slot_registration_failure in clogger.c. */
static _Atomic bool g_force_next_reg_slot_acquire_failure = false;

/* Snapshot of loop->reg_count taken right when a forced failure above fires,
 * before returning to the caller. With it a test can prove
 * ccol_event_loop_add's ORDERING directly and deterministically: acquire the
 * slot first, only then wire the reg into the registry. Without it a test
 * only sees the end state, which is the same for both orders (both make
 * ccol_event_loop_add return CCOL_EVENT_REG_INVALID with reg_count back at
 * 0, since rolling back a post-wiring failure restores reg_count too).
 * reg_count can already include THIS call's registration only if the wiring
 * ran before the slot acquire was even tried, so a snapshot of 0 proves the
 * wiring hadn't happened yet. Unlike the one-shot force flag above, this
 * value isn't reset between calls, and a test that never armed the force
 * flag never reads it. */
static _Atomic size_t g_forced_slot_acquire_failure_reg_count_snapshot = 0;

void _ccol_event_loop_test_force_next_reg_slot_acquire_failure(void) {
  atomic_store(&g_force_next_reg_slot_acquire_failure, true);
}

size_t _ccol_event_loop_test_last_forced_slot_acquire_failure_reg_count(void) {
  return atomic_load(&g_forced_slot_acquire_failure_reg_count_snapshot);
}

/* ccol_event_loop_add calls this once the new registration is live and
 * before the handle goes back to the caller; NULL means no hook. There's no
 * initializer because Clang 11 refuses any initializer on an atomic function
 * pointer, and static storage starts out null anyway. */
static void (*_Atomic g_evl_add_before_return_hook)(void);

void _ccol_event_loop_test_set_add_before_return_hook(void (*hook)(void)) {
  atomic_store(&g_evl_add_before_return_hook, hook);
}

/* Test-only. Lock and unlock the write side of loop's reg_slot_rwlock
 * directly, bypassing every public API function, in the same style as
 * _ccol_circq_test_lock_mutex_for_tests. A test can hold the write side from
 * a thread OTHER than the one that will call fork(), for exactly as long as
 * it wants. That's the scenario that deterministically exercises the
 * reg_slot_rwlock write-lock reinit in the in_child branch of
 * _cthreadcomm_atfork_release_impl: prepare() waits for the window to end
 * and takes the write side itself, and the child, whose single thread has a
 * new TID, can't release it with a plain _ccol_rw_lock_unlock.
 *
 * The lock call resolves loop exactly ONCE on purpose and keeps that
 * resolve's pin for as long as the write lock stays held, so the later
 * unlock call doesn't resolve loop again. A second _ccol_event_loop_resolve
 * from the unlock side would need ccol_event_loop_slot_table's process-wide
 * mutex, which the forking thread holds from before Phase 1 of
 * _cthreadcomm_atfork_prepare for as long as it is itself blocked waiting for
 * THIS write lock. That's a real AB-BA deadlock and not a theoretical one:
 * the forking thread gets stuck in _cthreadcomm_atfork_prepare's
 * _ccol_rw_lock_wrlock while the holder is stuck resolving loop for its own
 * unlock call.
 *
 * So the lock call hands the resolved struct _ccol_event_loop_s* on to the
 * unlock call as an opaque pointer and the second resolve disappears. Every
 * real public API function here works the same way, resolving once per call
 * and holding raw for the whole call; the only difference is that this test
 * hook's single logical operation, holding the lock across a window the test
 * times from outside, is split over two calls for the test's convenience. */
void *_ccol_event_loop_test_wrlock_reg_slot_for_tests(ccol_event_loop loop) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return NULL;
  _ccol_rw_lock_wrlock(raw->reg_slot_rwlock);
  return raw;
}

void _ccol_event_loop_test_wrunlock_reg_slot_for_tests(void *resolved_loop) {
  struct _ccol_event_loop_s *raw = (struct _ccol_event_loop_s *)resolved_loop;
  if (!raw) return;
  _ccol_rw_lock_unlock(raw->reg_slot_rwlock);
  _ccol_event_loop_resolve_unpin(raw);
}
#endif

/* Allocates a fresh slot in loop->reg_slots for reg, or reuses a freed one,
 * and returns the resulting ccol_event_reg handle, or 0 when it can't
 * allocate. ccol_event_loop_add calls it BEFORE wiring reg into the fd or
 * queue registry; that function's comment explains why this order, and not
 * the reverse, keeps a failed slot acquire cheap to unwind. Unlike in
 * ccol_event_loop's constructor, where a failed slot acquire has to unwind a
 * poller thread that is already running, a reg not yet wired into any
 * registry has nothing else to unwind. It sets reg->self_slot_idx. The caller
 * must not hold loop->reg_slot_rwlock, because this takes its write side: it
 * changes reg_slots and reg_free_indices, and the cvector_push_back below can
 * also reallocate reg_slots' backing storage, which a concurrent
 * _ccol_event_reg_resolve reader must never see half-moved. */
#ifdef RUNNING_UNIT_TESTS
/* Test-only. Makes the next free-index push in _ccol_event_reg_slot_release
   report a failure, then disarms itself. No caller-supplied allocator can
   fail exactly that one call, because reg_free_indices gets a non-zero
   capacity at loop creation and a push into spare capacity never
   allocates. */
static _Atomic bool g_fail_next_reg_free_index_push = false;

void _ccol_event_loop_force_next_reg_free_index_push_failure_for_tests(void) {
  atomic_store(&g_fail_next_reg_free_index_push, true);
}

/* Test-only. How many registration slots this loop has ever allocated; a
   recovered lost index must not make the number grow. */
size_t _ccol_event_loop_reg_slot_count_for_tests(ccol_event_loop loop) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return 0;
  _ccol_rw_lock_rdlock(raw->reg_slot_rwlock);
  size_t n = cvector_elem_count(raw->reg_slots);
  _ccol_rw_lock_unlock(raw->reg_slot_rwlock);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}
#endif

/* Recovers one slot index _ccol_event_reg_slot_release couldn't push back
   onto reg_free_indices (see loop->reg_lost_indices). The caller holds
   reg_slot_rwlock's write side and has found reg_free_indices empty, which
   is what makes the predicate unambiguous: a released slot is either on the
   free list or lost, never both, and a slot in use has a non-NULL ptr. It
   stays noinline so the ordinary acquire path, which only evaluates the
   reg_lost_indices comparison, keeps its own code shape. When a scan finds
   nothing it zeroes the counter, so a wrong count can never make every later
   acquire pay for a scan that can't succeed. */
static __attribute__((noinline)) bool _ccol_event_reg_reclaim_lost_index_locked(
    struct _ccol_event_loop_s *loop, uint32_t *out_idx) {
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
    struct _ccol_event_loop_s *loop, ccol_event_reg_s *reg) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_force_next_reg_slot_acquire_failure)) {
    atomic_store(&g_force_next_reg_slot_acquire_failure, false);
    atomic_store(&g_forced_slot_acquire_failure_reg_count_snapshot,
                 atomic_load(&loop->reg_count));
    return 0;
  }
#endif
  _ccol_rw_lock_wrlock(loop->reg_slot_rwlock);
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
      _ccol_rw_lock_unlock(loop->reg_slot_rwlock);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(loop->reg_slots) - 1;
    slot = (ccol_event_reg_slot_t *)cvector_at(loop->reg_slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that would collide with the reserved
   * "invalid handle" sentinel 0, which happens after about 2^32 reuses of
   * this exact slot index. The identical guard in
   * _ccol_event_loop_handle_slot_acquire explains why. */
  if (slot->generation == 0) slot->generation++;
  slot->ptr = reg;
  slot->in_use = true;
  reg->self_slot_idx = idx;
  ccol_event_reg h =
      ((ccol_event_reg)idx << 32) | (ccol_event_reg)slot->generation;
  reg->self_handle = h;
  _ccol_rw_lock_unlock(loop->reg_slot_rwlock);
  return h;
}

/* Resolves h against loop's reg slot table and pins the result against a
 * concurrent free. Returns NULL for an h of 0, for garbage, and for a slot
 * that is free right now or has been reused by a later acquire (wrong
 * generation). On success the caller MUST call
 * _ccol_event_reg_resolve_unpin(result) exactly once, as soon as it stops
 * touching the resolved ccol_event_reg_s*. It mirrors
 * _ccol_event_loop_resolve exactly, for one loop's table instead of the
 * process-wide one.
 *
 * It takes reg_slot_rwlock's read side, since it only reads the index, the
 * generation and ptr and never changes the table, so concurrent resolves can
 * run in parallel instead of queueing behind one lock. These resolves are the
 * hot path, called by ccol_event_loop_pause, _resume and _modify at the full
 * request rate (reg_slot_rwlock's field comment says why that matters). The
 * read side still excludes _ccol_event_reg_slot_acquire and
 * _ccol_event_reg_slot_release, which take the write side, and that's what
 * keeps a reader here from ever seeing reg_slots half-moved by a
 * reallocation from a concurrent cvector_push_back. */
static ccol_event_reg_s *_ccol_event_reg_resolve(
    struct _ccol_event_loop_s *loop, ccol_event_reg h) {
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  _ccol_rw_lock_rdlock(loop->reg_slot_rwlock);
  ccol_event_reg_s *raw = NULL;
  if (idx < cvector_elem_count(loop->reg_slots)) {
    ccol_event_reg_slot_t *slot =
        (ccol_event_reg_slot_t *)cvector_at(loop->reg_slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* The increment happens while we STILL hold reg_slot_rwlock's read side,
   * exactly like _ccol_event_loop_resolve's pending_resolve_count increment.
   * The only thing that could make raw unsafe to touch here is its slot being
   * marked not in use, which ccol_event_loop_remove does under this same
   * rwlock's write side. The check above just confirmed the slot is in use,
   * and the write side can never overlap this read side. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  _ccol_rw_lock_unlock(loop->reg_slot_rwlock);
  return raw;
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. Makes the next _ccol_event_reg_resolve_unpin call sleep for a
 * caller-chosen time between reading removed and handlers.on_removed (where
 * the still-held resolve pin guarantees raw is safe to touch) and
 * decrementing pending_resolve_count, then disarms itself.
 *
 * That lets a test hold a resolve pin open deterministically, long enough
 * for a concurrent ccol_event_loop_remove on another thread to mark the same
 * reg removed and defer its free first. It widens maybe_needs_wake's
 * read-before-decrement window (see that variable's comment inside
 * _ccol_event_reg_resolve_unpin), normally tiny, enough for a test to land a
 * concurrent ccol_event_loop_remove inside it, and the test then always hits
 * the same case: the snapshot goes stale, and EVENT_LOOP_RECLAIM_RETRY_MS's
 * bounded retry, not this unpin's ping, reclaims the reg and fires
 * on_removed. */
static _Atomic uint32_t g_delay_next_reg_resolve_unpin_ms = 0;

void _ccol_event_loop_test_delay_next_reg_resolve_unpin_ms(uint32_t ms) {
  atomic_store(&g_delay_next_reg_resolve_unpin_ms, ms);
}

/* Test-only. The unpin takes the delay with an exchange placed after its read
 * of removed, so true here means a caller that armed the delay has a thread
 * inside that window, with the read done and the pin still held. */
bool _ccol_event_loop_test_reg_resolve_unpin_delay_taken_for_tests(void) {
  return atomic_load(&g_delay_next_reg_resolve_unpin_ms) == 0u;
}
#endif

static void _ccol_event_reg_resolve_unpin(struct _ccol_event_loop_s *loop,
                                          ccol_event_reg_s *raw) {
  /* This read comes before the decrement below, while this call's still-held
   * pin unconditionally guarantees raw is safe to touch. removed and
   * handlers.on_removed only matter for one decision, whether to ping
   * reclaim_wake_efd afterwards (see the comment at that decision below). The
   * decrement can make raw eligible for a concurrent free, and after that
   * reading either field isn't safe. With removed false this stays one cheap
   * load and no syscall, which is by far the common case, an unpin of a live
   * registration: ccol_event_loop_modify, _pause, _resume and
   * ccol_event_loop_reg_generation are all much hotter paths than
   * ccol_event_loop_remove in practice, and each of them unpins a raw that is
   * almost always live. */
  bool maybe_needs_wake =
      atomic_load(&raw->removed) && raw->handlers.on_removed;

#ifdef RUNNING_UNIT_TESTS
  /* See g_delay_next_reg_resolve_unpin_ms's comment. It sits after the read
   * above and before the decrement below, with this call's pin held either
   * way, which from this function's point of view looks exactly like a real
   * but unusually slow caller of ccol_event_loop_modify, _pause, _resume or
   * ccol_event_loop_reg_generation. */
  uint32_t delay_ms = atomic_exchange(&g_delay_next_reg_resolve_unpin_ms, 0u);
  if (delay_ms > 0) {
    struct timespec ts = {.tv_sec = delay_ms / 1000,
                          .tv_nsec = (long)(delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
  }
#endif

  /* A bare atomic decrement with no lock.
   * _ccol_event_loop_reclaim_pending_frees polls this value instead of
   * waiting on a condition variable, so there's no lost wakeup to guard
   * against, the same reasoning as in _ccol_event_loop_resolve_unpin. */
  int prev = atomic_fetch_sub(&raw->pending_resolve_count, 1);

  /* Best-effort latency optimization only, not what makes things correct.
   * It pings reclaim_wake_efd (see that fd's field comment) when this unpin
   * LOOKS like it just made a reg newly eligible for reclaim, meaning one
   * that is already removed and has an on_removed handler, and the early
   * maybe_needs_wake snapshot is the only evidence for that.
   *
   * maybe_needs_wake is read before the decrement above because raw is only
   * guaranteed safe to touch up to there (see this function's comment above).
   * A concurrent ccol_event_loop_remove can land strictly between that read
   * and the decrement, and since ccol_event_loop_modify, _pause and _resume
   * all do real work between the resolve and the unpin, not just a couple of
   * instructions, it really can happen. Such a removal makes the snapshot
   * stale, so this condition can miss exactly the case where a ping matters
   * most.
   *
   * Reading raw's fields AFTER the decrement would fix the staleness, but it
   * reopens the very use-after-free this pin exists to prevent:
   * ccol_event_loop_remove's unconditional ping in
   * _ccol_event_loop_remove_finish may already have woken poller_thread,
   * which can then free raw between this call's decrement and any later read
   * of it.
   *
   * Instead, EVENT_LOOP_RECLAIM_RETRY_MS's bounded epoll_wait retry keeps the
   * case this ping misses correct (see _ccol_event_loop_reclaim_pending_frees
   * and _ccol_event_loop_thread_fn), and that retry needs no snapshot of raw
   * at all. A stale miss here costs at most one extra retry interval of
   * latency, never an indefinite hang. */
  if (prev == 1 && maybe_needs_wake) _eventfd_notify(loop->reclaim_wake_efd);
}

/* Marks reg's slot not in use and bumps its generation, so every later
 * _ccol_event_reg_resolve of the handle value ccol_event_loop_add returned
 * for it fails at once, and pushes the index onto the free list for a future
 * reg to reuse. ccol_event_loop_remove calls it right where reg is logically
 * removed, which is independent of, and always well before, the real free
 * of the ccol_event_reg_s struct (see
 * _ccol_event_loop_reclaim_pending_frees). Slot and struct have separate
 * lifecycles, linked only by reg->self_slot_idx, so a brand new registration
 * can reuse this slot index the moment this returns, and that reuse can
 * never race the original reg's deferred free, which may still be
 * pending. */
static void _ccol_event_reg_slot_release(struct _ccol_event_loop_s *loop,
                                         ccol_event_reg_s *reg) {
  _ccol_rw_lock_wrlock(loop->reg_slot_rwlock);
  ccol_event_reg_slot_t *slot =
      (ccol_event_reg_slot_t *)cvector_at(loop->reg_slots, reg->self_slot_idx);
  slot->in_use = false;
  slot->ptr = NULL;
  /* A failed push leaves this index released but off the free list, so we
     count it and the acquire path can recover it; without the count the slot
     would be lost for as long as this loop lives. */
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_fail_next_reg_free_index_push, false)) {
    loop->reg_lost_indices++;
    _ccol_rw_lock_unlock(loop->reg_slot_rwlock);
    return;
  }
#endif
  if (cvector_push_back(loop->reg_free_indices, &reg->self_slot_idx) !=
      ccol_success) {
    loop->reg_lost_indices++;
  }
  _ccol_rw_lock_unlock(loop->reg_slot_rwlock);
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. Makes the next initialisation of a queue registration's wait
   mutex in _ccol_event_reg_create report a failure, then disarms itself. No
   caller-supplied allocator can reach that failure, because initialising a
   mutex allocates nothing through the procs. */
static _Atomic bool g_fail_next_reg_sync_init = false;

void _ccol_event_loop_test_fail_next_reg_sync_init(void) {
  atomic_store(&g_fail_next_reg_sync_init, true);
}
#endif

/* Builds a new registration's struct. On failure it returns NULL with
 * *rv_out set to ccol_not_enough_memory when the allocation failed, or to
 * ccol_unexpected_failure when initialising the wait mutex or the wait
 * condition variable failed, so the caller can report each class with its
 * own message. */
static ccol_event_reg_s *_ccol_event_reg_create(struct _ccol_event_loop_s *loop,
                                                ccol_selectable sel,
                                                ccol_event_handlers_t handlers,
                                                void *arg,
                                                ccol_retval_t *rv_out) {
  ccol_event_reg_s *reg =
      _ccol_mem_calloc(loop->m_procs, 1, sizeof(ccol_event_reg_s));
  if (!reg) {
    *rv_out = ccol_not_enough_memory;
    return NULL;
  }
  reg->sel = sel;
  reg->handlers = handlers;
  reg->arg = arg;
  atomic_init(&reg->refcount, 1);
  atomic_init(&reg->removed, false);
  atomic_init(&reg->paused, false);
  atomic_init(&reg->pending_resolve_count, (size_t)0);
  reg->bridge_efd = -1;
  /* wait_mtx and wait_cond are initialised here, unconditionally and exactly
   * once, for every non-fd registration (every queue and ccol_channel
   * registration), whether or not _ccol_event_loop_add_queue ever runs to
   * completion, because ccol_event_loop_add's event_entry allocation can
   * fail and return before _ccol_event_loop_add_queue is even called. That's
   * what lets _ccol_event_reg_free be the SOLE owner of destroying these two
   * on every path, a later successful removal as well as any failure at
   * registration time, with no risk of a second pthread_mutex_destroy or
   * pthread_cond_destroy (undefined behaviour in POSIX) and no risk of
   * destroying memory nothing initialised. See the comment of
   * _ccol_event_reg_free. */
  if (sel.type != ccol_selectable_fd) {
    /* On either failure reg is freed directly and not through
     * _ccol_event_reg_free, since wait_mtx and wait_cond aren't validly
     * initialised at that point and that function's _ccol_mutex_destroy and
     * _ccol_cond_var_destroy calls on them would be undefined behaviour. On
     * every OTHER path _ccol_event_reg_free must be the sole owner of their
     * teardown, for the reason in this function's doc comment above. */
    bool mutex_failed;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_exchange(&g_fail_next_reg_sync_init, false))
      mutex_failed = true;
    else
#endif
      mutex_failed = _ccol_mutex_init(reg->wait_mtx) != 0;
    if (mutex_failed) {
      _ccol_mem_free(loop->m_procs, reg);
      *rv_out = ccol_unexpected_failure;
      return NULL;
    }
    if (_ccol_cond_var_init(reg->wait_cond) != 0) {
      _ccol_mutex_destroy(reg->wait_mtx);
      _ccol_mem_free(loop->m_procs, reg);
      *rv_out = ccol_unexpected_failure;
      return NULL;
    }
  }
  return reg;
}

/* This function alone owns two teardown steps for reg: destroying wait_mtx
 * and wait_cond, which _ccol_event_reg_create above initialised, and closing
 * bridge_efd, which _ccol_event_loop_add_queue opened. It runs in two
 * situations, the final free of a live registration after its removal once
 * the deferred-free reclamation allows it, and a ccol_event_loop_add call
 * that fails partway through registering a queue or ccol_channel selectable.
 * Neither _ccol_event_loop_add_queue's failure paths nor any other function
 * may destroy these fields. Doing it here, exactly once, whichever caller
 * gets here, is what rules out a double destroy; if
 * _ccol_event_loop_add_queue's failure paths tore them down too, this
 * function would tear the same fields down a second time right after. */
/* reg->owning_entry is set exactly once, only on the success path of
 * _ccol_event_loop_add_fd or _ccol_event_loop_add_queue (see the end of
 * either), and nothing clears it afterwards. So it's NULL at every call site
 * here that tears down a reg ccol_event_loop_add never wired live; no
 * dispatch can ever have seen such a reg, so arg's owner has nothing to hear
 * about. And it's non-NULL at every call site that frees a reg that WAS live
 * at some point: an ordinary removal reaching
 * _ccol_event_loop_reclaim_pending_frees once its refcount and its
 * pending_resolve_count are both 0, and a reg still registered when
 * __ccol_event_loop_destroy's teardown walk sweeps it up. That's how
 * on_removed fires exactly once for every registration that ever became real
 * and never for one that didn't, with one check right here and no call site
 * having to know which kind it has.
 *
 * Every call site gets here holding none of this module's locks: the
 * collection and dispatch locks are entry-scoped and nothing here touches
 * them, and reg's own bookkeeping, wait_mtx and wait_cond, is only destroyed
 * below, never locked. So on_removed is free to take an application-level
 * lock of its own, even one this same registration's other callbacks take,
 * with no risk of a lock-order cycle against anything in this module. */
static void _ccol_event_reg_free(struct _ccol_event_loop_s *loop,
                                 ccol_event_reg_s *reg) {
  if (reg->owning_entry && reg->handlers.on_removed)
    reg->handlers.on_removed(reg->arg);

  if (reg->sel.type != ccol_selectable_fd) {
    _ccol_mutex_destroy(reg->wait_mtx);
    _ccol_cond_var_destroy(reg->wait_cond);
    if (reg->bridge_efd >= 0) close(reg->bridge_efd);
  }
  _ccol_mem_free(loop->m_procs, reg);
}

/* The epoll interest bits one direction of an fd entry adds to the entry's
 * combined mask, or 0 when the direction has no registration or its
 * registration is paused right now. dir is the direction asked about, always
 * the slot the caller read reg from.
 *
 * Only a registration with a real on_readable handler adds EPOLLIN, and only
 * one with a real on_writable handler adds EPOLLOUT. epoll is
 * level-triggered here, so readiness nothing can consume comes back on every
 * single epoll_wait call for ever and the reactor burns a whole thread on
 * it; with the default num_reactor_threads of 1 that's the only thread, and
 * every other registration on the loop starves as well.
 *
 * An error-only registration is exactly that case: on_error set and the
 * direction's handler NULL, the caller's way of saying "tell me when this fd
 * dies, I want neither to read it nor to write it". A healthy connected
 * socket is writable from the instant it's registered, so EPOLLOUT on it
 * would spin without the peer doing anything at all.
 *
 * EPOLLERR and EPOLLHUP are added unconditionally, since epoll reports them
 * for a registered fd whether or not the mask asks. For a read direction
 * EPOLLRDHUP goes in next to them, so a peer closing its end still reaches
 * on_error: a plain TCP close() raises EPOLLIN and EPOLLRDHUP without
 * EPOLLHUP (which needs both directions down), so EPOLLRDHUP is the only bit
 * that reports it to a registration with no reader. */
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

/* Byte size of one event_entry allocation for loop. In a loop with a
 * dispatch_pool every entry carries the storage of its own dispatch job as
 * the tail of the same block (see _ccol_event_loop_entry_job). It's defined
 * below that job's type. */
static size_t _ccol_event_loop_entry_alloc_size(
    const struct _ccol_event_loop_s *loop);

/* Registers a new fd direction for reg in stripe idx, which the caller,
 * ccol_event_loop_add, computed from sel.fd before taking any lock. If sel.fd
 * already has an event_entry (its other direction is registered), the
 * interest is combined with EPOLL_CTL_MOD; otherwise a fresh entry is created
 * with EPOLL_CTL_ADD. A direction another reg already occupies is rejected
 * with ccol_not_permitted. On success it sets reg->owning_entry and
 * reg->stripe_idx, plus entry->stripe_idx for a new entry. The caller already
 * holds loop->stripes[idx].lock. */
static ccol_retval_t _ccol_event_loop_add_fd(struct _ccol_event_loop_s *loop,
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
    if (_ccol_mutex_init(entry->dispatch_lock) != 0) {
      _ccol_mem_free(loop->m_procs, entry);
      return ccol_unexpected_failure;
    }
    atomic_init(&entry->removed, false);
    atomic_init(&entry->refcount, (size_t)0);
    /* Minted once per NEW entry, never for a second direction joining an
     * already registered fd; that case takes the !new_entry path below and
     * shares the existing entry's generation, which is right since both
     * directions stand for one logical connection. */
    entry->generation = atomic_fetch_add(&loop->fd_generation_counter, 1) + 1;
  }

  ccol_event_reg_s **slot = (reg->sel.dir == ccol_select_read)
                                ? &entry->as.fd.read_reg
                                : &entry->as.fd.write_reg;
  if (*slot != NULL) {
    if (new_entry) {
      _ccol_mutex_destroy(entry->dispatch_lock);
      _ccol_mem_free(loop->m_procs, entry);
    }
    return ccol_not_permitted;
  }
  *slot = reg;

  /* EPOLLONESHOT applies only when dispatch_pool exists, i.e. with
   * num_reactor_threads > 1, where collection (on poller_thread) and
   * dispatch (on a ctpool worker) are decoupled. Without it, poller_thread
   * would see a still-ready, not yet dispatched fd on every later epoll_wait
   * for as long as the backlog lasts and mint an endless stream of redundant
   * dispatch jobs, a real resource-exhaustion and livelock risk under
   * sustained load rather than a rare corner case.
   * _ccol_event_loop_rearm_entry_locked re-arms after each dispatch job.
   *
   * EPOLLONESHOT MUST NOT be set with num_reactor_threads == 1, whose
   * dispatch, _ccol_event_loop_handle_event, never re-arms anything. There
   * collection and dispatch are one synchronous call on one thread and
   * nothing else can re-arm, so EPOLLONESHOT would silently stop delivering
   * every event for this fd after the first one. */
  uint32_t mask = loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0;
  mask |= _fd_reg_interest_bits(entry->as.fd.read_reg, ccol_select_read);
  mask |= _fd_reg_interest_bits(entry->as.fd.write_reg, ccol_select_write);

  ccol_poll_event ev;
  ev.data.ptr = entry;
  ev.events = mask;
  /* The choice keys off entry->epoll_added rather than new_entry, because
   * entry->epoll_added says whether the fd is CURRENTLY in loop->epfd's
   * interest set. An earlier _ccol_event_loop_rearm_entry_locked call may
   * have taken an existing entry out with EPOLL_CTL_DEL, which it does when
   * every then-live direction of the entry is paused (see the field comment
   * of event_entry.epoll_added), and a second direction joining the entry in
   * that state needs EPOLL_CTL_ADD, not EPOLL_CTL_MOD, since MOD on an fd
   * that isn't registered at the moment fails with ENOENT. The reg just put
   * into *slot above is always freshly unpaused (see _ccol_event_reg_create),
   * so mask always carries real interest bits here, whatever a sibling
   * direction's paused state is. */
  /* A new entry goes into the fd index BEFORE epoll_ctl publishes it. The
   * index is only read under the stripe lock this function holds, so the
   * insert stays invisible until this call returns, and the one step here
   * that can fail for lack of memory runs while nothing else can reach the
   * entry. Once epoll_ctl has added the fd the poller may already hold the
   * entry pointer from an epoll_wait batch and only a removal's deferred-free
   * path may free it; freeing it right after the publish would be a
   * use-after-free in the poller. */
  if (new_entry && !_fd_registry_insert(stripe, fd, entry)) {
    *slot = NULL;
    _ccol_mutex_destroy(entry->dispatch_lock);
    _ccol_mem_free(loop->m_procs, entry);
    return ccol_not_enough_memory;
  }

  int ctl_op = entry->epoll_added ? CCOL_POLL_CTL_MOD : CCOL_POLL_CTL_ADD;
  bool want_in = (mask & CCOL_POLL_IN) != 0;
  if (ctl_op == CCOL_POLL_CTL_MOD && want_in != entry->armed_in)
    ev.events |= CCOL_POLL_READ_MODE_CHANGE;
  if (ccol_poll_ctl(loop->epfd, ctl_op, fd, &ev, &entry->poll_info) < 0) {
    *slot = NULL;
    if (new_entry) {
      /* A failed EPOLL_CTL_ADD published nothing, so the entry is still
       * private to this call. */
      _fd_registry_remove(stripe, fd);
      _ccol_mutex_destroy(entry->dispatch_lock);
      _ccol_mem_free(loop->m_procs, entry);
    }
    return ccol_unexpected_failure;
  }
  entry->epoll_added = true;
  entry->armed_in = want_in;

  reg->owning_entry = entry;
  reg->stripe_idx = idx;
  reg->generation = entry->generation;
  return ccol_success;
}

/* Peeks whether sel is ready right now for its own direction. sel is a
 * resolved circq or dynq selectable, never an fd, and this is the one shared
 * definition of "ready" for a queue selectable in this file: msg_count > 0
 * for read, and room to send or sends turned off for write (a send on a queue
 * whose sends are off returns ccol_not_permitted at once, so a writer needs
 * to hear about that state). Like every other reader of msg_count, max_size
 * and writing_disabled here, the caller already holds the resolved queue's
 * mutex. _ccol_event_loop_add_queue's already_ready check and
 * _ccol_event_loop_queue_cascade_notify_next below both use it, so they can
 * never drift apart about what "ready" means. */
static bool _queue_sel_is_ready(ccol_selectable *sel) {
  if (sel->type == ccol_selectable_circq) {
    ccol_circular_queue *cq = sel->cq;
    return (sel->dir == ccol_select_read)
               ? cq->msg_count > 0
               : (cq->msg_count < cq->max_size || cq->writing_disabled);
  }
  ccol_dynamic_queue *dq = sel->dq;
  return (sel->dir == ccol_select_read)
             ? dq->msg_count > 0
             : (dq->writing_disabled || dq->msg_count < ccol_max_elem_count);
}

/* Sets up a queue-backed registration: allocates its dedicated bridge
 * eventfd, registers it with the loop's persistent epoll instance, and links
 * reg's embedded waiter_node into the queue's waiter list for good (where
 * ccol_select's per-call nodes are transient). So the queue's existing
 * notify_one_sel_waiter and notify_all_sel_waiters, which
 * ccol_circq_send_zc, ccol_circq_recv_zc, ccol_dynmq_send_zc,
 * ccol_dynmq_recv_zc and the enable_sending and disable_sending functions
 * call, wake this registration too. */
static ccol_retval_t _ccol_event_loop_add_queue(struct _ccol_event_loop_s *loop,
                                                event_entry *entry,
                                                ccol_event_reg_s *reg) {
  reg->bridge_efd = ccol_wakefd_create();
  if (reg->bridge_efd < 0) {
    /* _ccol_event_reg_create initialised reg->wait_mtx and reg->wait_cond,
     * and they must NOT be torn down here: on any non-success return from
     * this function the caller, ccol_event_loop_add, unconditionally calls
     * _ccol_event_reg_free(reg), their one and only owner. Destroying them
     * here as well would be a second pthread_mutex_destroy and
     * pthread_cond_destroy, undefined behaviour in POSIX. */
    return ccol_unexpected_failure;
  }
  reg->waiter_node.efd = reg->bridge_efd;

  /* The comment in _ccol_event_loop_add_fd about EPOLLONESHOT only with a
   * dispatch_pool applies here for exactly the same reason, for a single
   * direction. Two regs never share a bridge eventfd, so there's no
   * combined-mask concern like in the fd case. */
  ccol_poll_event ev;
  ev.data.ptr = entry;
  ev.events = CCOL_POLL_IN | (loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0);
  if (ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_ADD, reg->bridge_efd, &ev, NULL) <
      0) {
    /* Only the fd itself needs closing here: it was never handed to epoll, so
     * nothing else will ever close it. wait_mtx and wait_cond are left alone
     * so _ccol_event_reg_free tears them down exactly once, for the same
     * reason as in the eventfd() failure branch above. reg->waiter_node.efd
     * is reset next to reg->bridge_efd, since a few lines up it was set to
     * the same fd value just closed. A stale value there would be harmless,
     * because this failure path never links the node into any queue's waiter
     * list and nothing reads it before reg is freed, but the reset keeps
     * reg->bridge_efd and reg->waiter_node.efd from ever disagreeing about
     * whether a real open fd exists. */
    close(reg->bridge_efd);
    reg->bridge_efd = -1;
    reg->waiter_node.efd = -1;
    return ccol_unexpected_failure;
  }

  _ccol_mutex_t *q_mtx;
  ccol_sel_waiter **q_head;
  ccol_sel_waiter **q_rotor; /* Unused here. A link of a brand-new node never
                              * needs to touch the rotor. See the doc
                              * comment of notify_one_sel_waiter. */
  _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
  (void)q_rotor;

  _ccol_mutex_lock(*q_mtx);
  /* A one-element-array calling convention. _sel_link_waiter indexes into
   * nodes[i] because ccol_select_timed always has a real array its caller
   * owns, while ccol_event_loop has exactly one standalone waiter_node per
   * registration; with nodes = &reg->waiter_node, nodes[0] is
   * reg->waiter_node. */
  _sel_link_waiter(0, &reg->waiter_node, &reg->wait_mtx, &reg->wait_cond,
                   &reg->wait_ready, q_head);
  /* For a real fd, an epoll_ctl(ADD) on a kernel object that is already
   * readable shows up in the very next epoll_wait, since epoll tracks the
   * resource's live state and not just its edges. This bridge eventfd only
   * rings on a FUTURE notify_one_sel_waiter call, though, so without the step
   * below a message already sitting in the queue before this registration
   * existed would go unnoticed until the next send. So we trigger the eventfd
   * ourselves when the queue is already in the target state, under q_mtx so
   * the check agrees with the link above. The reactor thread's next
   * epoll_wait then picks it up and dispatches through the completely
   * standard drain and try_recv path, so a callback still only ever runs
   * from there. */
  bool already_ready = _queue_sel_is_ready(&reg->sel);
  /* The wake goes through _notify_waiter like every other wake of this node
   * so wait_ready records it; a raw eventfd write here would leave the flag
   * clear and the next send would write a second, redundant wake. */
  if (already_ready) _notify_waiter(&reg->waiter_node);
  _ccol_mutex_unlock(*q_mtx);

  return ccol_success;
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only instrumentation for the cascade step below (see
 * _ccol_event_loop_test_delay_next_queue_cascade_us and
 * _ccol_event_loop_test_queue_cascade_counts in cthreadcomm.h). Atomic,
 * because the cascade runs on poller_thread or a dispatch worker while a
 * test thread reads these at the same time. */
static _Atomic uint32_t g_queue_cascade_test_delay_us = 0;
static _Atomic uint64_t g_queue_cascade_begun_for_tests = 0;
static _Atomic uint64_t g_queue_cascade_finished_for_tests = 0;
#endif

/* Decides who gets told about a queue that is ready for reg's direction once
 * reg's callback has returned, by which time this dispatch's callback has had
 * its chance to consume or produce. If the queue isn't ready any more,
 * nobody is woken.
 *
 * The decision is made for reg alone, from its own dispatch's progress:
 *
 *   - The dispatch moved a message: the queue's message count went down for
 *     a read registration, or up for a write registration, between consuming
 *     the wake (see _queue_reg_consume_wake) and this call. Then reg itself
 *     is woken again, so a registration that moves messages keeps getting
 *     dispatched for as long as it does and the queue stays ready, exactly
 *     like a level-triggered fd. reg's successor in the list (the node
 *     linked right after it) is woken too, so a backlog also reaches the
 *     queue's other listeners.
 *
 *   - The dispatch moved nothing: the wake goes on to reg's successor, or to
 *     the head of the list when reg is the last node, so a listener that
 *     declines a ready queue never keeps a message from a sibling that would
 *     take it. The forwarded wake carries a hop count one above the one reg
 *     consumed (see ccol_sel_waiter's fwd_hops field), and forwarding stops
 *     once the count reaches the list length. So while nobody moves anything
 *     a wake visits each listener at most once, and listeners that all
 *     decline a ready queue can't keep waking each other for ever.
 *
 * Neither rule depends on where reg sits in the list. A registration that
 * moves messages keeps its own dispatches going whether it's the head, the
 * tail or somewhere in between, and an idle sibling can neither end nor
 * swallow them. Every wake has a bounded chain behind it: re-waking reg needs
 * a message that moved, and each forward without progress raises the hop
 * count, which only a fresh event (a send, a receive, a state change, a new
 * registration or a leaving listener) resets.
 *
 * Another thread may move a message between the consume and this call. A
 * move in reg's direction (a receive for a reader, a send for a writer)
 * counts as progress here, costing reg at most one extra dispatch per such
 * move. A move in the other direction can hide reg's progress from the
 * count, but that operation notifies a waiter of reg's direction, and a wake
 * without progress then walks the list until it reaches a listener that
 * moves messages, reg included. If a modify changed reg's direction during
 * the dispatch the count means nothing, and the dispatch counts as one that
 * moved nothing.
 *
 * Both dispatch paths, _ccol_event_loop_handle_event and
 * _ccol_event_loop_dispatch_job_fn, call this once, right after the callback
 * returns, while the collection's extra refcount still pins reg, so nothing
 * can free reg from under this call.
 *
 * We need it because a ccol_event_loop registration doesn't behave like a
 * ccol_select() waiter. A ccol_select() waiter is transient: it always
 * unlinks itself before checking readiness again and forwards the wake when
 * it leaves a ready queue (see deregister_sel_waiter). A ccol_event_loop
 * registration's waiter_node instead stays linked for as long as the
 * registration lives. One eventfd write covers every send up to the next
 * dispatch, so a callback taking one message per call leaves the rest of a
 * burst in the queue, and maybe no further send ever comes; without the
 * re-wake that backlog would sit there with nobody woken. And without the
 * forward, a wake landing on a listener that declines the queue would end
 * there while a sibling that would take the message waits for a send that
 * may never come.
 *
 * loop->stripes[reg->stripe_idx].lock is held across BOTH the "is reg still
 * registered" test and the whole q_mtx critical section, because the queue's
 * memory has no refcount of its own and can stop existing in between.
 * Locking a queue mutex needs the QUEUE to be alive, which is strictly more
 * than reg being alive: the collection's refcount bump keeps reg alive here
 * no matter what happens to the queue, whose lifetime the application owns.
 * This module's documented lifetime rule for the queue is to call
 * ccol_event_loop_remove and then destroy the queue at once (see the doc
 * comment of ccol_circular_queue_destroy). So an unlocked read of
 * reg->removed followed by an unprotected _ccol_mutex_lock(*q_mtx) would
 * lock freed memory whenever that destroy lands in between. The gap may be
 * tiny and it still happens, since the OS can deschedule this thread for an
 * arbitrarily long time at exactly that point.
 *
 * The stripe lock makes the pair indivisible, for two reasons that must
 * hold together:
 *
 *   - _ccol_event_loop_remove_unlink's queue branch does its whole
 *     splice-and-mark-removed step under this same stripe lock, so reg's
 *     removed state can't change while we hold the lock. A concurrent
 *     ccol_event_loop_remove either hasn't started yet, so removed reads
 *     false and stays false here, or has fully finished, so removed reads
 *     true and nothing below runs.
 *
 *   - removed being false means reg is linked into the queue's
 *     sel_{read,write}_waiters_head list, which is exactly when
 *     __ccol_circular_queue_destroy and __ccol_dynamic_queue_destroy refuse
 *     to destroy a queue: they abort through _ccol_assert BEFORE destroying
 *     the mutex or freeing the struct. So an application racing a destroy
 *     against this function can't free the queue from under the
 *     _ccol_mutex_lock below; the assert traps it as the caller bug it
 *     already is.
 *
 * The lock order is stripe->lock, then the queue mutex, then the target
 * waiter's wait_mtx inside _notify_waiter, matching both the
 * stripe-then-queue nesting of _ccol_event_loop_add_queue and
 * _ccol_event_loop_remove_unlink and _notify_waiter's queue-then-wait_mtx
 * nesting. The caller already holds entry->dispatch_lock, and dispatch lock
 * before stripe lock is the order _ccol_event_loop_handle_event uses itself.
 * Nothing in this module ever takes a queue mutex or a wait_mtx before a
 * stripe lock, so no cycle shows up.
 *
 * Reading reg->waiter_node.next is only safe under the queue's mutex, the
 * same invariant _sel_link_waiter and _sel_unlink_waiter_locked rely on
 * throughout this file. A concurrent ccol_event_loop_remove()'s
 * _sel_unlink_waiter_locked updates the pointers of reg->waiter_node's
 * NEIGHBOURS but never resets reg->waiter_node.next and .prev themselves once
 * reg is spliced out, so a stale read of reg->waiter_node.next after that
 * removal can dereference a node that is unlinked by then and maybe freed.
 *
 * The removed reg itself doesn't need this, since a removal hands on any
 * wake the removed reg may be holding (see _ccol_event_loop_remove_unlink).
 * It only matters for an in-flight dispatch that was collected before a
 * concurrent removal finished. */
static void _ccol_event_loop_queue_cascade_notify_next(
    struct _ccol_event_loop_s *loop, ccol_event_reg_s *reg,
    ccol_select_dir dispatched_dir, size_t queue_count_before,
    uint32_t wake_hops) {
  ccol_event_loop_stripe_t *stripe = &loop->stripes[reg->stripe_idx];
  _ccol_mutex_lock(stripe->lock);

#ifdef RUNNING_UNIT_TESTS
  /* Both sit inside the stripe lock, so a test that sees the begun counter
   * knows this call already holds that lock (see their declarations in
   * cthreadcomm.h). */
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

  /* Once this lock is held there's no need for a second, inner check of
   * removed: _ccol_event_loop_remove_unlink is the only writer of
   * reg->removed and it holds this very lock across the write, so the value
   * read here can't change under the block below. */
  if (!atomic_load(&reg->removed)) {
    _ccol_mutex_t *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
    /* This cascade step is independent of the queue's round-robin rotor and
     * never moves it (the doc comment of notify_one_sel_waiter explains what
     * the rotor is for). Leaving it alone is right, because the rotor tracks
     * who is due for the next brand-new arrival, which is a different
     * question from this function's, whether there is a backlog to surface
     * right now. */
    (void)q_rotor;

    _ccol_mutex_lock(*q_mtx);
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
    _ccol_mutex_unlock(*q_mtx);
  }

#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_queue_cascade_finished_for_tests, (uint64_t)1);
#endif
  _ccol_mutex_unlock(stripe->lock);
}

#ifdef RUNNING_UNIT_TESTS
void _ccol_event_loop_test_delay_next_queue_cascade_us(uint32_t us) {
  atomic_store(&g_queue_cascade_test_delay_us, us);
}

void _ccol_event_loop_test_queue_cascade_counts(uint64_t *begun,
                                                uint64_t *finished) {
  if (begun) *begun = atomic_load(&g_queue_cascade_begun_for_tests);
  if (finished) *finished = atomic_load(&g_queue_cascade_finished_for_tests);
}
#endif

/* Checks a ccol_selectable for ccol_event_loop_add. */
static ccol_retval_t _ccol_event_loop_validate_add_args(
    struct _ccol_event_loop_s *loop, ccol_selectable *sel) {
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
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("Invalid or stale ccol_event_loop handle");
    return CCOL_EVENT_REG_INVALID;
  }

  ccol_retval_t validate = _ccol_event_loop_validate_add_args(raw, &sel);
  if (validate != ccol_success) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("Invalid arguments to ccol_event_loop_add");
    _ccol_event_loop_resolve_unpin(raw);
    return CCOL_EVENT_REG_INVALID;
  }

  ccol_retval_t create_rv = ccol_success;
  ccol_event_reg_s *reg =
      _ccol_event_reg_create(raw, sel, handlers, arg, &create_rv);
  if (!reg) {
    if (err_str)
      *err_str =
          create_rv == ccol_not_enough_memory
              ? _CCOL_ERR_STR(
                    "ccol_event_loop_add: failed to allocate the registration")
              : _CCOL_ERR_STR(
                    "ccol_event_loop_add: a system call failed (the "
                    "initialisation of a mutex or a condition variable)");
    _ccol_event_loop_resolve_unpin(raw);
    return CCOL_EVENT_REG_INVALID;
  }

  /* The slot acquire happens BEFORE reg is wired into the fd or queue
   * registry below, never after, because the reverse order is a real, if
   * narrow, use-after-free. Once reg is wired and the target stripe's lock is
   * released, reg is fully live and dispatchable through the raw
   * ccol_event_reg_s* in the registry alone: the poller thread needs no
   * public handle or slot to collect and dispatch it, and with
   * num_reactor_threads > 1 that dispatch can hand reg->arg to a ctpool
   * worker asynchronously.
   *
   * A slot acquire can fail in one way, an allocation failure while growing
   * loop->reg_slots. A failure past that point makes ccol_event_loop_add
   * report failure to a caller who then believes no registration was ever
   * created, has no reason to think a callback could already be running, or
   * queued, against reg->arg, and may free reg->arg immediately. All it takes
   * to get there is some unlucky timing, readiness arriving in the brief
   * window between the wiring and the slot acquire, plus an ordinary
   * allocation failure, with no misuse at all.
   *
   * Acquiring the slot first closes this completely, because reg can only
   * become dispatchable once it has a valid handle. A failed slot acquire then
   * means nothing was ever wired into any registry, and there's nothing to
   * roll back beyond releasing the slot just acquired and freeing reg
   * directly (see the rv != ccol_success branch below).
   *
   * The order must also match the only other place these two locks nest.
   * _cthreadcomm_atfork_prepare locks every live loop's reg_slot_rwlock on
   * its write side (through _ccol_event_reg_slot_acquire below) in its own
   * Phase 1, before it takes any of that loop's stripe locks, so before
   * stripe->lock is ever taken here. Taking the slot before the stripe lock
   * keeps this function consistent with that existing reg_slot_rwlock before
   * stripe->lock order, while the other way round would add a new
   * stripe->lock before reg_slot_rwlock edge that races the existing one and
   * risks an AB-BA deadlock against a concurrent fork(). So it's more than a
   * matter of style. */
  ccol_event_reg h = _ccol_event_reg_slot_acquire(raw, reg);
  if (h == 0) {
    if (err_str)
      *err_str = _CCOL_ERR_STR(
          "ccol_event_loop_add: failed to allocate the registration handle "
          "slot");
    _ccol_event_reg_free(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return CCOL_EVENT_REG_INVALID;
  }

  /* The stripe index is computed before any lock is taken, from data we
   * already have. An fd selectable uses sel.fd, a pure function of the fd,
   * so a second ccol_event_loop_add for the fd's other direction computes
   * the same stripe independently and finds the existing entry through that
   * stripe's chmap. A queue or ccol_channel selectable, which has no such
   * consistency rule, uses the round-robin cursor loop->next_queue_stripe.
   * See the comment of ccol_event_loop_stripe_t. */
  size_t idx =
      (sel.type == ccol_selectable_fd)
          ? _stripe_index_for_fd(raw, sel.fd)
          : (atomic_fetch_add(&raw->next_queue_stripe, 1) % raw->num_stripes);
  ccol_event_loop_stripe_t *stripe = &raw->stripes[idx];

  _ccol_mutex_lock(stripe->lock);

  /* A shut-down loop dispatches nothing any more, so a registration on it
   * would never run, and a queue registration would also take wakes that the
   * queue's other listeners need. The test runs under the stripe lock, which
   * the shutdown takes after setting shutting_down in order to unlink the
   * queue registrations, so a registration is either refused here or linked
   * before that walk and unlinked by it. */
  bool shut_down = atomic_load(&raw->shutting_down);
  ccol_retval_t rv;
  if (shut_down) {
    rv = ccol_not_permitted;
  } else if (sel.type == ccol_selectable_fd) {
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
      if (_ccol_mutex_init(entry->dispatch_lock) != 0) {
        _ccol_mem_free(raw->m_procs, entry);
        rv = ccol_unexpected_failure;
      } else {
        atomic_init(&entry->removed, false);
        atomic_init(&entry->refcount, (size_t)0);
        /* Queue and ccol_channel selectables never share an entry. It's one
         * to one and nothing gets combined, so every ccol_event_loop_add
         * call here mints a fresh generation; unlike on the fd path, a
         * second direction never joins an existing entry. */
        entry->generation =
            atomic_fetch_add(&raw->fd_generation_counter, 1) + 1;
        rv = _ccol_event_loop_add_queue(raw, entry, reg);
        if (rv == ccol_success) {
          reg->owning_entry = entry;
          reg->stripe_idx = idx;
          reg->generation = entry->generation;
          _loop_queue_list_add(stripe, reg);
        } else {
          _ccol_mutex_destroy(entry->dispatch_lock);
          _ccol_mem_free(raw->m_procs, entry);
        }
      }
    }
  }

  if (rv == ccol_success) atomic_fetch_add(&raw->reg_count, 1);

  _ccol_mutex_unlock(stripe->lock);

  if (rv != ccol_success) {
    /* One message per failure class, so a caller can tell a direction
     * another registration already holds (a usage error no retry will fix)
     * from running out of memory or a failed system call. */
    if (err_str) {
      if (shut_down)
        *err_str = _CCOL_ERR_STR("ccol_event_loop_add: the loop is shut down");
      else if (rv == ccol_not_permitted)
        *err_str = _CCOL_ERR_STR(
            "ccol_event_loop_add: direction already in use on this fd");
      else if (rv == ccol_not_enough_memory)
        *err_str = _CCOL_ERR_STR(
            "ccol_event_loop_add: failed to allocate the registration");
      else
        *err_str = _CCOL_ERR_STR(
            "ccol_event_loop_add: a system call failed (epoll_ctl, eventfd "
            "or a mutex initialisation)");
    }
    /* reg already has a slot, acquired above before any wiring was tried, but
     * it was never linked into any registry: every failure path inside
     * _ccol_event_loop_add_fd and _ccol_event_loop_add_queue leaves *slot or
     * entry.as.reg back at NULL and undoes its own partial epoll_ctl work
     * before returning. So there's nothing for ccol_event_loop_remove's
     * two-phase unlink and finish steps to undo, and no dispatch can ever
     * have seen reg.
     *
     * So we release the slot directly, in the same order as
     * ccol_event_loop_remove (slot first, before reg becomes eligible for
     * reclaim). No other thread can have resolved this handle, since h was
     * never returned to anyone. Then reg is freed outright instead of going
     * through the pending-free and epoch machinery, which only exists to
     * protect in-flight dispatches of an already live registration. */
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
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
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

/* Computes, fresh, the epoll interest mask entry wants right now and applies
 * it: for an fd from the live state of entry->as.fd.read_reg and
 * entry->as.fd.write_reg, and for a queue or ccol_channel from
 * entry->as.reg, never from a cached snapshot of some earlier moment.
 *
 * In the fd case every live direction may be paused right now, and the mask
 * then has no real interest bit at all apart from EPOLLONESHOT when there is
 * a dispatch_pool. The kernel reports EPOLLERR and EPOLLHUP unconditionally,
 * whatever the registered mask says, so an empty mask can never silence an
 * fd that is (or gets into) an error or hang-up condition while paused, and
 * a level-triggered epoll_wait then reports it for ever.
 * _ccol_event_loop_run_callback re-checks reg->paused and skips the callback
 * every time, but nothing stops the reactor from seeing and collecting that
 * fd again on every single epoll_wait, an unbounded CPU-spinning busy loop.
 * It would also quietly break ccol_event_loop_pause's promise that no
 * callback fires, exactly as after a remove. A really removed registration
 * gets no further wake thanks to EPOLL_CTL_DEL, but one with just a narrower
 * mask can't get that, since the mask can't opt out of ERR and HUP.
 *
 * So in that case we really issue EPOLL_CTL_DEL and track the result in
 * entry->epoll_added. An fd removed this way needs EPOLL_CTL_ADD, not
 * EPOLL_CTL_MOD, once some direction wants real interest again, because MOD
 * on an fd that's not in the interest set at the moment fails with ENOENT.
 * See the field comment of event_entry.epoll_added.
 *
 * Five call sites share this function: ccol_event_loop_modify's direction
 * flip, ccol_event_loop_remove's partial-removal branch (one direction of an
 * fd remains), ccol_event_loop_pause's mask-exclude step,
 * ccol_event_loop_resume's mask-include step, and
 * _ccol_event_loop_dispatch_job_fn's post-dispatch EPOLLONESHOT re-arm. The
 * comment of _ccol_event_loop_add_fd explains why every registration needs
 * EPOLLONESHOT when there is a dispatch_pool.
 *
 * Having one shared implementation is what keeps all five race-free against
 * each other: whichever runs later under the same stripe lock always computes
 * and applies "what must be armed right now" fresh, and that answer includes
 * each fd-direction reg's paused state, not just whether a slot is occupied.
 * A concurrent ccol_event_loop_modify and a worker's re-arm for the other
 * direction of the same fd, or a ccol_event_loop_pause racing a dispatch
 * job's post-dispatch re-arm, can therefore only ever be redundant with each
 * other, never racy, and neither ever replays a stale mask the other has
 * already moved past.
 *
 * Nothing happens when entry->removed is set: the entry then has no live
 * registration left, because it was fully removed while a dispatch job for it
 * was in flight, and the application may have closed or reused its fd by
 * now. The generation counter protects callers from that same hazard
 * elsewhere, and this flag brings the same protection to the module's own
 * re-arm. Nothing happens either for a queue entry whose entry->as.reg a
 * concurrent ccol_event_loop_remove has set to NULL; that function's comment
 * explains why it clears the field before deferring the entry's free.
 *
 * The caller already holds loop->stripes[entry->stripe_idx].lock. */
static void _ccol_event_loop_rearm_entry_locked(struct _ccol_event_loop_s *loop,
                                                event_entry *entry) {
  if (atomic_load(&entry->removed)) return;

  ccol_poll_event ev;
  ev.data.ptr = entry;

  if (entry->is_fd) {
    uint32_t mask = loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0;
    mask |= _fd_reg_interest_bits(entry->as.fd.read_reg, ccol_select_read);
    mask |= _fd_reg_interest_bits(entry->as.fd.write_reg, ccol_select_write);

    /* EPOLLONESHOT alone, or a plain 0, isn't real interest: every live
     * direction is paused right now. So we really drop the fd from the
     * interest set instead of handing the kernel a hollow mask, which
     * couldn't suppress the unconditional EPOLLERR and EPOLLHUP monitoring.
     * See this function's doc comment. */
    bool wants_interest = (mask & ~(uint32_t)CCOL_POLL_ONESHOT) != 0;
    if (wants_interest) {
      ev.events = mask;
      int op = entry->epoll_added ? CCOL_POLL_CTL_MOD : CCOL_POLL_CTL_ADD;
      bool want_in = (mask & CCOL_POLL_IN) != 0;
      if (op == CCOL_POLL_CTL_MOD && want_in != entry->armed_in)
        ev.events |= CCOL_POLL_READ_MODE_CHANGE;
      if (ccol_poll_ctl(loop->epfd, op, entry->fd, &ev, &entry->poll_info) ==
          0) {
        entry->epoll_added = true;
        entry->armed_in = want_in;
      }
    } else if (entry->epoll_added) {
      ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_DEL, entry->fd, NULL,
                    &entry->poll_info);
      entry->epoll_added = false;
    }
  } else if (entry->as.reg) {
    ev.events = CCOL_POLL_IN | (loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0);
    ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_MOD, entry->as.reg->bridge_efd, &ev,
                  NULL);
  }
}

/* This deliberately doesn't take entry->dispatch_lock as well, because that
 * would need a read of reg->owning_entry before the stripe lock confirms
 * reg->removed is false, exactly the unsafe read pattern owning_entry's doc
 * comment warns about (see ccol_event_reg's stripe_idx field).
 * entry->dispatch_lock is only ever taken after that stripe-lock-protected
 * confirmation, in _ccol_event_loop_handle_event, never before it.
 *
 * So the write to reg->sel.dir below really can happen at the same time as an
 * in-flight callback using *sel. That's a real race, unrelated to this
 * module's dispatch_lock, which neither causes nor closes it; the comment of
 * _dispatch_item.sel_snapshot explains what makes it safe instead. The
 * dispatch path snapshots reg->sel under the stripe lock at collection time
 * and never hands a callback a live pointer into reg->sel, and the write here
 * happens under the stripe lock as well, so it can never race a callback's
 * read. */
ccol_retval_t ccol_event_loop_modify(ccol_event_loop loop, ccol_event_reg reg_h,
                                     ccol_select_dir new_dir) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  if (new_dir != ccol_select_read && new_dir != ccol_select_write) {
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  /* Resolving reg_h through loop's reg slot table is what makes every field
   * read below safe: a stale or already removed handle is caught right here,
   * before anything is dereferenced, instead of by touching memory that may
   * already be freed. See the reg_slots field comment of struct
   * _ccol_event_loop_s. */
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
  _ccol_mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    _ccol_mutex_unlock(stripe->lock);
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (reg->sel.dir == new_dir) {
    /* The direction stays the same and a muted reg is watched again, as this
     * function's documentation promises for every successful call. The
     * confirmed removed == false makes owning_entry safe to read. */
    if (reg->muted) {
      reg->muted = false;
      _ccol_event_loop_rearm_entry_locked(raw, reg->owning_entry);
    }
    _ccol_mutex_unlock(stripe->lock);
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_success;
  }

  /* The check above just confirmed removed is false under this very stripe's
   * lock, the only lock under which removed can become true, so owning_entry
   * is certainly not freed yet here. */
  event_entry *entry = reg->owning_entry;
  ccol_event_reg_s **target_slot = (new_dir == ccol_select_read)
                                       ? &entry->as.fd.read_reg
                                       : &entry->as.fd.write_reg;
  if (*target_slot != NULL) {
    _ccol_mutex_unlock(stripe->lock);
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

  _ccol_mutex_unlock(stripe->lock);
  _ccol_event_reg_resolve_unpin(raw, reg);
  _ccol_event_loop_resolve_unpin(raw);
  return ccol_success;
}

/* Shared checks for ccol_event_loop_pause and ccol_event_loop_resume, which
 * are both fd only, the same restriction as ccol_event_loop_modify. A queue or
 * ccol_channel registration's bridge eventfd has no use for "stop caring for
 * a while but keep the registration", since nothing outside this module ever
 * touches a queue's fd directly, while chttpserver and chttpclient do read
 * and write a connection's fd themselves during a paused window.
 *
 * Both must confirm reg->removed again under reg's stripe lock before
 * touching owning_entry, as ccol_event_loop_modify does for the same reason
 * (see that function's comment on reg->stripe_idx).
 *
 * Returns ccol_success with *out_entry set to reg->owning_entry when the
 * caller should carry on. Any other return value means the caller unlocks
 * and returns that value unchanged. */
static ccol_retval_t _ccol_event_loop_pause_resume_validate_locked(
    struct _ccol_event_loop_s *loop, ccol_event_reg_s *reg,
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
 * ccol_event_loop_remove unregisters the registration completely and defers
 * its free, while ccol_event_loop_pause leaves reg whole: it keeps its slot
 * on the underlying fd's entry, counts toward ccol_event_loop_reg_count and
 * keeps the same ccol_event_loop_reg_generation. The function only recomputes
 * the fd's combined epoll interest mask and leaves reg out of it.
 *
 * It's the cheap alternative to ccol_event_loop_remove followed later by
 * ccol_event_loop_add when the same logical registration comes back, a
 * connection that goes to a worker thread for blocking body I/O and returns
 * to the reactor for its next request, for example. A pause costs no heap
 * allocation, no free and no fd-registry chmap churn, just one epoll_ctl
 * call, where a remove and add pair costs two, a DEL and then an ADD. For a
 * registration that keeps coming back, pausing is the better choice.
 *
 * While reg is paused no on_readable, on_writable or on_error callback fires
 * for it, exactly as after a remove. The other direction on the same fd, if
 * any, is not affected. Pausing a reg that is already paused succeeds and
 * does nothing.
 *
 * @param loop The ccol_event_loop that the registration belongs to
 * @param reg  The registration to pause
 *
 * @return ccol_success on success
 * @return ccol_invalid_args when loop or reg is NULL, when reg is a queue or
 * ccol_channel registration, and when another thread removed reg at the same
 * time.
 *
 * @note Thread-safe. It can run concurrently with ccol_event_loop_remove, and
 *       from inside a callback running on the reactor thread.
 *
 * @see ccol_event_loop_resume
 */
ccol_retval_t ccol_event_loop_pause(ccol_event_loop loop,
                                    ccol_event_reg reg_h) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;

  ccol_event_reg_s *reg = _ccol_event_reg_resolve(raw, reg_h);
  if (!reg) {
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol_event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  _ccol_mutex_lock(stripe->lock);

  event_entry *entry = NULL;
  ccol_retval_t rv =
      _ccol_event_loop_pause_resume_validate_locked(raw, reg, &entry);
  if (rv != ccol_success) {
    _ccol_mutex_unlock(stripe->lock);
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return rv;
  }

  if (!atomic_load(&reg->paused)) {
    atomic_store(&reg->paused, true);
    _ccol_event_loop_rearm_entry_locked(raw, entry);
  }

  _ccol_mutex_unlock(stripe->lock);
  _ccol_event_reg_resolve_unpin(raw, reg);
  _ccol_event_loop_resolve_unpin(raw);
  return ccol_success;
}

/* @brief Resume event delivery for a registration previously paused by
 * ccol_event_loop_pause.
 *
 * Recomputes the fd's combined epoll interest mask with reg included.
 * Resuming a reg that isn't paused right now, because it never was or has
 * already been resumed, succeeds and does nothing. That deliberately matches
 * ccol_event_loop_modify's "already in the requested state" behaviour,
 * because an error would be worse: a caller can race its own pause and resume
 * pair against a concurrent ccol_event_loop_remove, and shouldn't have to
 * tell "already resumed" from "nothing to do" by the return value alone.
 *
 * @param loop The ccol_event_loop that the registration belongs to
 * @param reg  The registration to resume
 *
 * @return ccol_success on success
 * @return ccol_invalid_args when loop or reg is NULL, when reg is a queue or
 * ccol_channel registration, and when another thread removed reg at the same
 * time, for example when the connection closes while the caller thinks it
 * owns a paused registration to resume.
 *
 * @note Thread-safe. It can run concurrently with ccol_event_loop_remove, and
 *       from inside a callback running on the reactor thread.
 *
 * @see ccol_event_loop_pause
 */
ccol_retval_t ccol_event_loop_resume(ccol_event_loop loop,
                                     ccol_event_reg reg_h) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;

  ccol_event_reg_s *reg = _ccol_event_reg_resolve(raw, reg_h);
  if (!reg) {
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol_event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  _ccol_mutex_lock(stripe->lock);

  event_entry *entry = NULL;
  ccol_retval_t rv =
      _ccol_event_loop_pause_resume_validate_locked(raw, reg, &entry);
  if (rv != ccol_success) {
    _ccol_mutex_unlock(stripe->lock);
    _ccol_event_reg_resolve_unpin(raw, reg);
    _ccol_event_loop_resolve_unpin(raw);
    return rv;
  }

  if (atomic_load(&reg->paused) || reg->muted) {
    atomic_store(&reg->paused, false);
    reg->muted = false;
    _ccol_event_loop_rearm_entry_locked(raw, entry);
  }

  _ccol_mutex_unlock(stripe->lock);
  _ccol_event_reg_resolve_unpin(raw, reg);
  _ccol_event_loop_resolve_unpin(raw);
  return ccol_success;
}

/* ccol_event_loop_remove can't free an event_entry synchronously. The
 * epoll_ctl(DEL) and the clearing of the entry slot stop any FUTURE
 * epoll_wait from returning a new event for it, but that isn't enough:
 * epoll_wait can return a batch of several ready events in one call that a
 * reactor thread then works through one by one, so this entry's event may sit
 * at a later index of a batch that thread fetched into its local stack array
 * before this remove() call. Freeing the entry here would race that
 * not-yet-processed index against ccol_event_loop_remove, a real
 * use-after-free (reading a stale event_entry* after a concurrent remove(),
 * which SIGSEGVs). epoll_ctl(DEL) can't invalidate an event the kernel has
 * already copied out to userspace.
 *
 * So the real free is deferred to a point where poller_thread can prove no
 * batch can reference the entry any more, between the end of one epoll_wait
 * batch and the start of the next, which closes the race. With a dispatch
 * pool (num_reactor_threads > 1) that batch boundary alone isn't enough,
 * because a ctpool worker may be in the middle of dispatching a job that
 * references this entry however far poller_thread has moved on. The big
 * comment above _ccol_event_loop_reclaim_pending_frees describes the
 * epoch-and-refcount scheme that covers both. This function only pushes the
 * node with its defer_gen snapshot stamped on; it doesn't decide when a free
 * is really safe.
 *
 * The push is a lock-free Treiber-stack push, because
 * loop->pending_entry_frees is loop-wide state rather than per stripe:
 * entries from every stripe go onto this one list, so no single stripe lock
 * could protect it. There's no ABA hazard, because a node only ever leaves
 * the list as part of claiming the WHOLE list at once with one
 * atomic_exchange (see _ccol_event_loop_reclaim_pending_frees); nodes are
 * never popped and freed one at a time while the list is shared. */
static void _ccol_event_loop_push_entry_free_node(
    struct _ccol_event_loop_s *loop, event_entry *entry) {
  event_entry *old_head = atomic_load(&loop->pending_entry_frees);
  do {
    entry->pending_free_next = old_head;
  } while (!atomic_compare_exchange_weak(&loop->pending_entry_frees, &old_head,
                                         entry));
}

static void _ccol_event_loop_defer_entry_free(struct _ccol_event_loop_s *loop,
                                              event_entry *entry) {
  atomic_store(&entry->removed, true);
  entry->defer_gen = atomic_load(&loop->poller_batch_gen);
  _ccol_event_loop_push_entry_free_node(loop, entry);
}

/* A ccol_event_reg can't be freed synchronously either, for a reason related
 * to but not the same as _ccol_event_loop_defer_entry_free's: a dispatch
 * callback may be in flight (refcount above 1 at removal time) and still need
 * reg after ccol_event_loop_remove returns. So the free waits until the
 * refcount really reaches 0, which the caller checks before ever calling this
 * function, and the memory stays valid for that callback to finish safely.
 *
 * Once a reg is deferred, the real reclaim also waits for
 * reg->pending_resolve_count to reach 0 (see struct _ccol_event_loop_s's
 * reg_slots field comment and _ccol_event_loop_reclaim_pending_frees). That
 * wait is what lets the documentation promise a graceful ccol_invalid_args,
 * not undefined behaviour, when ccol_event_loop_remove and
 * ccol_event_loop_modify race each other on the same reg. The list has the
 * same lock-free Treiber-stack shape as
 * _ccol_event_loop_push_entry_free_node and
 * _ccol_event_loop_defer_entry_free. */
static void _ccol_event_loop_push_reg_free_node(struct _ccol_event_loop_s *loop,
                                                ccol_event_reg_s *reg) {
  ccol_event_reg_s *old_head = atomic_load(&loop->pending_reg_frees);
  do {
    reg->pending_free_next = old_head;
  } while (
      !atomic_compare_exchange_weak(&loop->pending_reg_frees, &old_head, reg));
}

static void _ccol_event_loop_defer_reg_free(struct _ccol_event_loop_s *loop,
                                            ccol_event_reg_s *reg) {
  _ccol_event_loop_push_reg_free_node(loop, reg);
}

/* Reclaims every entry/reg deferred by _ccol_event_loop_defer_entry_free /
 * _ccol_event_loop_defer_reg_free that is now provably safe to free, and
 * leaves the rest pending for a later attempt.
 *
 * In every configuration exactly one thread, poller_thread, calls
 * epoll_wait, so "safe to free" comes down to one scalar comparison.
 * poller_thread calls this at its between-batches point (see
 * _ccol_event_loop_thread_fn), after publishing its advanced
 * poller_batch_gen. An item is eligible once its defer_gen snapshot is older
 * than the current poller_batch_gen, meaning poller_thread has crossed a
 * between-batches point at least once since the deferral and so can't still
 * be in the middle of whatever batch it had in flight at the time. The item
 * was also taken out of the registry (epoll_ctl(DEL) or a cleared slot)
 * before it was deferred, so no FUTURE batch can reference it either.
 *
 * An entry needs one condition on top of the epoch one, refcount == 0 (see
 * the comment of event_entry.refcount). With num_reactor_threads > 1 a
 * ctpool worker's dispatch job can use an entry well after poller_thread has
 * moved past the entry's deferral epoch, and the epoch check alone says
 * nothing about that. A reg needs no extra check here, because its own
 * refcount mechanism means it is never deferred while a callback, inline or
 * on a worker, still needs it (see _ccol_event_loop_release_after_dispatch).
 *
 * It's lock-free: it claims each list's whole contents with one
 * atomic_exchange, sorts the items outside any lock into "free now" and "not
 * eligible yet", and pushes the not-yet-eligible ones back with the same
 * CAS-based push the original defer used. It does NOT restamp defer_gen,
 * because those items' original snapshot is what lets them make progress; a
 * new stamp would restart their eligibility clock every time a reclaim
 * attempt found them pending, which under sustained load could starve them
 * for ever.
 *
 * __ccol_event_loop_destroy uses the same logic in its unconditional form
 * (see _ccol_event_loop_free_all_pending) once it has fully joined
 * poller_thread and drained dispatch_pool, if there is one. No epoch or
 * refcount check is needed there, since no thread can reference anything any
 * more.
 *
 * Returns true when anything at all stays pending, i.e. got pushed back onto
 * either list instead of freed, and _ccol_event_loop_thread_fn uses that to
 * choose its NEXT epoll_wait timeout. A pending reg with a non-NULL
 * on_removed handler needs a bounded retry rather than an indefinite one,
 * because no ping is reliable: the ping comes from whichever call finally
 * drops its pending_resolve_count to 0, and it may not fire. The comment of
 * _ccol_event_reg_resolve_unpin explains how that snapshot can go stale
 * against a concurrent ccol_event_loop_remove, and also why reading raw's
 * fields AFTER this function's reclaim would just open the use-after-free
 * this epoch and refcount scheme exists to prevent. So the bounded retry here
 * is the correctness backstop, and the pings stay as a latency optimization
 * for the common case where they do fire. */
static bool _ccol_event_loop_reclaim_pending_frees(
    struct _ccol_event_loop_s *loop) {
  uint64_t reached = atomic_load(&loop->poller_batch_gen);

  event_entry *e = atomic_exchange(&loop->pending_entry_frees, NULL);
  event_entry *e_keep = NULL;
  while (e) {
    event_entry *next = e->pending_free_next;
    if (e->defer_gen < reached && atomic_load(&e->refcount) == 0) {
      _ccol_mutex_destroy(e->dispatch_lock);
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

  /* A reg is eligible when pending_resolve_count == 0, not by an epoch
   * comparison (see struct _ccol_event_loop_s's reg_slots field comment). Its
   * slot, if it had one, was released under loop->reg_slot_rwlock's write
   * side right at the deferral (see ccol_event_loop_remove's two-phase call
   * site), so no NEW resolve of it can succeed from then on. A
   * pending_resolve_count of 0 therefore means no thread anywhere holds a
   * pointer to this reg from an in-flight call (ccol_event_loop_modify,
   * _pause, _resume, _remove or ccol_event_loop_reg_generation), and the reg
   * can be freed outright with no further lock. */
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

/* The unconditional variant, used once every reactor thread has been joined,
 * i.e. after ccol_event_loop_shutdown returned. No thread is left that could
 * reference anything, so every remaining deferred item is safe to free
 * whatever its defer_gen says. It frees whatever the very last
 * _ccol_event_loop_reclaim_pending_frees call before the join left
 * pending. */
static void _ccol_event_loop_free_all_pending(struct _ccol_event_loop_s *loop) {
  event_entry *e = atomic_exchange(&loop->pending_entry_frees, NULL);
  while (e) {
    event_entry *next = e->pending_free_next;
    _ccol_mutex_destroy(e->dispatch_lock);
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

/* Phase 1 of ccol_event_loop_remove's shared core. It takes an already
 * resolved raw reg pointer, not a handle, and the public
 * ccol_event_loop_remove uses it after resolving and pinning reg with
 * _ccol_event_reg_resolve. ccol_event_loop_add has no use for it: that
 * function acquires reg's slot BEFORE wiring reg into the fd or queue
 * registry (see its own comment on _ccol_event_reg_slot_acquire), so a
 * wiring failure there always finds reg fully unlinked and this unlink logic
 * has nothing to do; in that case
 * ccol_event_loop_add releases the slot and frees reg directly.
 *
 * It unlinks reg from the fd or queue registry and marks it removed. It does
 * NOT touch the refcount and doesn't defer reg for reclaim yet; see phase 2,
 * _ccol_event_loop_remove_finish, and ccol_event_loop_remove's call site for
 * why that split matters. reg's slot, if any, must be released BETWEEN the
 * two phases, before reg goes onto loop->pending_reg_frees, or a second
 * resolve racing the slot release could succeed against a reg that's
 * already eligible for a real reclaim.
 *
 * Returns true when this call is the one that really did the removal
 * (reg->removed was false and is now true), and false when an earlier call
 * already removed reg, which is a graceful no-op; the caller must then not go
 * on to phase 2 and must not release a slot. */
static bool _ccol_event_loop_remove_unlink(struct _ccol_event_loop_s *raw,
                                           ccol_event_reg_s *reg) {
  ccol_event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  _ccol_mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    _ccol_mutex_unlock(stripe->lock);
    return false;
  }

  event_entry *entry = reg->owning_entry;

  if (reg->sel.type == ccol_selectable_fd) {
    ccol_event_reg_s **slot = (reg->sel.dir == ccol_select_read)
                                  ? &entry->as.fd.read_reg
                                  : &entry->as.fd.write_reg;
    *slot = NULL;
    if (entry->as.fd.read_reg == NULL && entry->as.fd.write_reg == NULL) {
      /* entry->epoll_added may already be false here if every direction was
       * paused, since _ccol_event_loop_rearm_entry_locked then issued its own
       * EPOLL_CTL_DEL (see that function's comment), so we only issue another
       * one while the fd is still registered. */
      if (entry->epoll_added)
        ccol_poll_ctl(raw->epfd, CCOL_POLL_CTL_DEL, entry->fd, NULL,
                      &entry->poll_info);
      _fd_registry_remove(stripe, entry->fd);
      _ccol_event_loop_defer_entry_free(raw, entry);
    } else {
      _ccol_event_loop_rearm_entry_locked(raw, entry);
    }
  } else {
    _ccol_mutex_t *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);

    /* The splice that unlinks reg from the queue's waiter list and the
     * removed=true store MUST happen in ONE SINGLE q_mtx critical section,
     * not two separate lock and unlock pairs on the same mutex. Once the
     * splice runs, reg->waiter_node.next and .prev are frozen: a splice only
     * updates reg's NEIGHBOURS' pointers, never reg's own, and reg isn't
     * linked any more for a later splice to touch (see
     * _sel_unlink_waiter_locked).
     *
     * Suppose removed were set to true only in a later, separate critical
     * section. A concurrent _ccol_event_loop_queue_cascade_notify_next(reg)
     * on another thread, in the middle of dispatching reg's already collected
     * callback (ccol_event_loop_remove doesn't wait for that to finish),
     * could lock q_mtx in the gap between the two. It would see removed as
     * false and read reg->waiter_node.next, an already stale value frozen at
     * whatever registration followed reg at splice time. A THIRD, independent
     * ccol_event_loop_remove() on that successor could complete and reclaim
     * it before cascade-notify gets there, since a ccol_event_reg's reclaim
     * has no epoch delay once its refcount and pending_resolve_count are both
     * 0, and cascade-notify would then call _notify_waiter() on freed memory.
     *
     * Doing both mutations in one critical section closes this. By the time
     * cascade-notify's lock(q_mtx) succeeds there are only two possible
     * states: the splice and removed=true have both happened together, so
     * removed reads true and nothing touches .next, or neither has, so the
     * queue and every registration linked to it are fully intact.
     *
     * That's what makes this module's documented pattern (call
     * ccol_event_loop_remove, then free or destroy the underlying resource
     * without waiting) safe for a QUEUE selectable too; fd selectables already
     * rely on it through _ccol_event_loop_run_callback's removed re-check.
     * _ccol_event_loop_queue_cascade_notify_next also touches this queue
     * after a dispatch, in a critical section that locks q_mtx, checks
     * removed, maybe notifies and unlocks q_mtx, and the mutex guarantees
     * that section either fully precedes or fully follows this one and never
     * overlaps it. If it precedes, the queue is obviously alive and so is
     * every registration reg's .next can point to. If it follows, removed is
     * visible as true the moment its own lock succeeds, since a mutex release
     * and acquire is itself a memory barrier, so it never touches
     * reg->waiter_node.next at all.
     *
     * The combination is load-bearing, not defensive: splitting the splice
     * and the removed=true store into two critical sections reliably
     * reproduces a heap-use-after-free under valgrind, reported as a
     * _ccol_mutex_lock on a destroyed queue's mutex, with the test pattern
     * "wait for the callback's signal, then remove and destroy the queue at
     * once from a different thread".
     *
     * The same critical section hands on any wake reg may hold. A send or a
     * receive wakes exactly one listener, and if that listener is reg and reg
     * leaves before a dispatch acted on the wake, the following dispatch sees
     * removed and does nothing, so the message would sit in the queue with
     * every other listener asleep until some later send. So the wake goes to
     * the next listener whenever the queue is still ready for reg's
     * direction, exactly as a leaving ccol_select() waiter does in
     * deregister_sel_waiter. If reg didn't actually hold a wake, it costs one
     * spurious dispatch of a listener that finds the queue ready anyway. */
    _ccol_mutex_lock(*q_mtx);
    /* A shutdown of the loop has already unlinked the node and handed on its
     * wake; see _ccol_event_loop_detach_queue_regs. */
    bool linked = !reg->waiter_detached;
    if (linked)
      _sel_unlink_waiter_locked(&reg->waiter_node, q_head, q_rotor);
    else
      (*_queue_sel_detached_count(&reg->sel))--;
    atomic_store(&reg->removed, true);
    if (linked && _queue_sel_is_ready(&reg->sel))
      notify_one_sel_waiter(q_head, q_rotor);
    _ccol_mutex_unlock(*q_mtx);

    ccol_poll_ctl(raw->epfd, CCOL_POLL_CTL_DEL, reg->bridge_efd, NULL, NULL);
    _loop_queue_list_remove(stripe, reg);
    /* entry itself is deferred below and its memory stays valid, but its
     * as.reg field must become NULL HERE, under stripe->lock, before that
     * deferral. Otherwise a stale batch entry could read entry->as.reg after
     * reg was deferred and freed, and get a live-looking pointer into the
     * pending-free list instead of a clean NULL. The fd branch above does the
     * same thing with *slot = NULL. */
    entry->as.reg = NULL;
    _ccol_event_loop_defer_entry_free(raw, entry);
  }

  if (reg->sel.type == ccol_selectable_fd) {
    atomic_store(&reg->removed, true);
  }

  _ccol_mutex_unlock(stripe->lock);
  return true;
}

/* Phase 2: decrements the refcount and, if that was the last reference,
 * defers reg for reclaim. It must run strictly AFTER reg's slot (if any)
 * was released, so reg is never both on the pending-free list and resolvable
 * through its old handle; _ccol_event_loop_remove_unlink's comment explains
 * why. */
static void _ccol_event_loop_remove_finish(struct _ccol_event_loop_s *raw,
                                           ccol_event_reg_s *reg) {
  atomic_fetch_sub(&raw->reg_count, 1);

  int prev = atomic_fetch_sub(&reg->refcount, 1);
  /* reg is deferred instead of freed right here because a callback may be in
   * flight and still need it. The comments of _ccol_event_loop_defer_reg_free
   * and _ccol_event_loop_reclaim_pending_frees describe the full reclaim
   * scheme, which waits for reg->pending_resolve_count to reach 0 (see struct
   * _ccol_event_loop_s's reg_slots field comment). */
  if (prev == 1) _ccol_event_loop_defer_reg_free(raw, reg);

  /* Ping poller_thread's reclaim_wake_efd unconditionally (see that fd's
   * field comment). By now the removal has ALWAYS deferred something:
   * _ccol_event_loop_remove_unlink deferred reg's event_entry in both of its
   * branches, except for the fd case where the other direction is still
   * registered and the entry deliberately stays live, and the refcount drop
   * above may have deferred reg itself too.
   *
   * Only poller_thread ever reclaims deferred memory, at its own
   * between-batches point, and neither an epoll_ctl(EPOLL_CTL_DEL) nor an
   * epoll_ctl(EPOLL_CTL_ADD) of an fd that isn't ready at the moment wakes a
   * blocked epoll_wait. On an otherwise idle loop nothing else would bring
   * poller_thread back at all, so the deferred entry and reg would sit on
   * loop->pending_entry_frees and pending_reg_frees indefinitely, keeping
   * reg->bridge_efd open (only _ccol_event_reg_free closes it), and an add
   * and remove cycle repeating on an idle loop would leak file descriptors
   * without bound.
   *
   * The bounded EVENT_LOOP_RECLAIM_RETRY_MS retry can't replace this, since
   * it only kicks in once poller_thread has woken at least once after the
   * deferral and seen something pending. The ping is also what gives an
   * on_removed handler its small, bounded latency. With a dispatch in flight
   * (prev != 1) poller_thread is already running by construction, and the
   * ping is a cheap, harmless no-op it drains the next time epoll_wait
   * returns. */
  _eventfd_notify(raw->reclaim_wake_efd);
}

ccol_retval_t ccol_event_loop_remove(ccol_event_loop loop,
                                     ccol_event_reg reg_h) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  ccol_event_reg_s *reg = _ccol_event_reg_resolve(raw, reg_h);
  if (!reg) {
    _ccol_event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (_ccol_event_loop_remove_unlink(raw, reg)) {
    /* This invalidates reg's handle at once for every later resolve attempt,
     * BEFORE phase 2 below can make reg eligible for a real reclaim. The
     * comment of _ccol_event_loop_remove_unlink explains why this order is
     * the race-free one and releasing the slot after phase 2 isn't. It's
     * skipped when this call found reg already removed by an earlier call,
     * which has done it already. */
    _ccol_event_reg_slot_release(raw, reg);
    _ccol_event_loop_remove_finish(raw, reg);
  }

  /* Safe even though reg may have just been deferred for reclaim: this
   * call's pin from the successful resolve above is still held, and that's
   * exactly what guarantees _ccol_event_loop_reclaim_pending_frees can't
   * have freed it yet. */
  _ccol_event_reg_resolve_unpin(raw, reg);
  _ccol_event_loop_resolve_unpin(raw);
  return ccol_success;
}

size_t ccol_event_loop_reg_count(ccol_event_loop loop) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_size;
  size_t n = atomic_load(&raw->reg_count);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}

/* One reg collected for dispatch under the entry's stripe lock and acted on
 * once that lock is released. */
typedef struct _dispatch_item {
  ccol_event_reg_s *reg;
  bool is_error;
  bool is_readable;
  bool is_writable;
  /* A snapshot of reg->sel taken under the stripe lock at collection time
   * (see _ccol_event_loop_handle_event), not a live pointer into reg->sel.
   * ccol_event_loop_modify updates reg->sel.dir under the stripe lock only;
   * its comment explains why it can't also take entry->dispatch_lock, which
   * would conflict with the lock order of the safe owning_entry read.
   *
   * Handing a callback a live `&reg->sel` pointer AFTER releasing the stripe
   * lock would mean a real unsynchronized concurrent read and write on
   * reg->sel.dir between an in-flight callback and a ccol_event_loop_modify
   * call from some other thread, which ThreadSanitizer reports. It can happen
   * even with a single reactor thread, since the documentation lets any
   * thread call ccol_event_loop_modify while a dispatch is running.
   *
   * One snapshot, copied under the stripe lock along with the rest of the
   * collection, makes that race impossible, and the callback sees a
   * self-consistent selectable as of collection time. The documented
   * contract of ccol_event_readable_fn and ccol_event_writable_fn still
   * holds: sel->dir shows the registration's current direction, which
   * ccol_event_loop_modify may have changed. reg->sel is read fresh on every
   * dispatch, so a ccol_event_loop_modify that finished before this
   * collection is seen correctly, and one running entirely in parallel with
   * an in-flight callback resolves deterministically to whatever direction
   * was current at collection time. */
  ccol_selectable sel_snapshot;
  /* Queue-backed items only: the queue's message count when the dispatch
   * consumed the wake, right before the callback. See
   * _queue_reg_consume_wake and _ccol_event_loop_queue_cascade_notify_next. */
  size_t queue_count_before;
  /* Queue-backed items only: the hop count of the wake the dispatch
   * consumed. See ccol_sel_waiter's fwd_hops field. */
  uint32_t wake_hops;
} _dispatch_item;

/* This file's two collection paths, _ccol_event_loop_handle_event and
 * _ccol_event_loop_poller_collect, are separate on purpose. The first serves
 * num_reactor_threads == 1 and takes entry->dispatch_lock; the second serves
 * the dispatch-pool path and must stay free of every dispatch-pool concern
 * (see the design comment above _ccol_event_loop_poller_collect). The split
 * is about LOCKING and about keeping the one-thread path light.
 *
 * The RULE that decides what an epoll event means for one registration isn't
 * part of that split. It's the same rule on both paths, and if it drifted
 * between them the reactor would dispatch a different callback depending on
 * how many threads the caller configured. The two helpers below hold that
 * rule in one place, so the paths can't disagree about it while keeping
 * their separate locking.
 *
 * Both are static inline and take only values the caller already has.
 * _ccol_fd_reg_takes_event's decision goes straight into _ccol_fd_item_fill,
 * so neither path computes it twice, and everything beyond plain readiness of
 * a direction with a handler (an error, a hang-up, and muting a registration
 * nothing can report to) is out of line and cold. */

/* Whether an event on one direction of an fd entry has anything to dispatch
 * to reg at all. The caller holds the entry's stripe lock.
 *
 * A removed registration is skipped (see the field comment of
 * ccol_event_reg_s.removed). Otherwise, in this order:
 *
 * - The direction is ready and the registration has a handler for it: the
 *   event goes to that handler even if the fd also carries an error, so
 *   bytes that arrived before the error aren't lost.
 * - The registration has on_error: the event goes there. That includes a
 *   ready direction with no handler of its own, which for a read
 *   registration means the peer hung up.
 * - The fd carries an error or a hang-up and the registration has a handler
 *   for its direction but no on_error: the event goes to that handler, as in
 *   libevent and libuv. Its read() or write() returns the error, or 0 at end
 *   of file, and reading or writing a socket clears a pending socket error
 *   (SO_ERROR), so a transient error such as a connected UDP socket's ICMP
 *   port unreachable doesn't stop delivery of the data that follows.
 *
 * An event none of the registration's handlers can take is reported to
 * nobody. The condition behind it may stay true and level-triggered epoll
 * would then report it again on every epoll_wait, so reg gets muted (see
 * _ccol_fd_reg_mute_locked). The check for a handler of the direction comes
 * first, so an ordinary readable or writable event pays nothing for this. */
static void _ccol_fd_reg_mute_locked(struct _ccol_event_loop_s *loop,
                                     event_entry *entry, ccol_event_reg_s *reg);

typedef enum {
  _FD_TAKE_NONE,      /* Nothing to dispatch to this registration. */
  _FD_TAKE_DIRECTION, /* Dispatch to the handler of its direction. */
  _FD_TAKE_ERROR,     /* Dispatch to on_error. */
} _fd_take;

static _fd_take _ccol_fd_reg_takes_condition(struct _ccol_event_loop_s *loop,
                                             event_entry *entry,
                                             ccol_event_reg_s *reg,
                                             bool is_write_direction);

static inline _fd_take _ccol_fd_reg_takes_event(
    struct _ccol_event_loop_s *loop, event_entry *entry, ccol_event_reg_s *reg,
    bool is_err, bool direction_ready, bool is_write_direction) {
  if (!reg || atomic_load(&reg->removed) || !(is_err || direction_ready))
    return _FD_TAKE_NONE;
  if (direction_ready &&
      (is_write_direction ? reg->handlers.on_writable != NULL
                          : reg->handlers.on_readable != NULL))
    return _FD_TAKE_DIRECTION;
  return _ccol_fd_reg_takes_condition(loop, entry, reg, is_write_direction);
}

/* The rest of _ccol_fd_reg_takes_event's rule, for an event that isn't plain
 * readiness of a direction with a handler: an error, a hang-up, or a ready
 * direction with no handler of its own (for a read registration, a peer that
 * hung up). The caller holds entry's stripe lock. It stays out of line and
 * cold so an ordinary readable or writable event keeps the short inlined
 * path above. */
static __attribute__((noinline, cold)) _fd_take _ccol_fd_reg_takes_condition(
    struct _ccol_event_loop_s *loop, event_entry *entry, ccol_event_reg_s *reg,
    bool is_write_direction) {
  if (reg->handlers.on_error) return _FD_TAKE_ERROR;
  /* No on_error. The caller let the event through, so either the fd carries
   * an error or a hang-up, or the direction is ready without a handler of its
   * own. In the first case the direction's handler takes the event, and its
   * own read() or write() reports the condition and, for a socket error,
   * consumes it. Without a handler for the direction nothing can take it. */
  if (is_write_direction ? reg->handlers.on_writable != NULL
                         : reg->handlers.on_readable != NULL)
    return _FD_TAKE_DIRECTION;
  _ccol_fd_reg_mute_locked(loop, entry, reg);
  return _FD_TAKE_NONE;
}

/* Mutes reg (see its field comment) and re-arms entry from live state, which
 * takes reg out of the interest mask while a sibling direction with handlers
 * stays armed. The caller holds entry's stripe lock.
 *
 * The re-arm happens right away on both dispatch paths. On the dispatch-pool
 * path it may re-arm an EPOLLONESHOT entry for which the caller is about to
 * submit a job for the sibling direction; an event the poller collects for
 * the entry before that job finishes is dropped by
 * _ccol_event_loop_poller_collect's in-flight check, and the job's
 * post-dispatch re-arm applies the live state again afterwards. It stays out
 * of line and cold so the dispatch paths that inline the collection keep
 * their shape. */
static __attribute__((noinline, cold)) void _ccol_fd_reg_mute_locked(
    struct _ccol_event_loop_s *loop, event_entry *entry,
    ccol_event_reg_s *reg) {
  reg->muted = true;
  _ccol_event_loop_rearm_entry_locked(loop, entry);
}

/* Fills item for one direction of an fd entry.
 *
 * EPOLLIN and EPOLLRDHUP don't exclude EPOLLERR and EPOLLHUP, and neither
 * does EPOLLOUT: the kernel can legitimately report both on one event. That
 * happens when a peer writes data and then immediately closes or resets the
 * connection, so the bytes really are in the receive buffer and readable
 * although the peer is gone. A socket that just went into an error state
 * also reads as writable, because write(2) on it returns at once instead of
 * blocking.
 *
 * Always putting the error first would therefore throw away readable data a
 * reader registered for, and starve a write-only registration of on_writable
 * for good, so the direction wins only where it has a handler to consume it.
 * An error with no ready direction goes to on_error if the registration has
 * one and otherwise to the direction's handler (see
 * _ccol_fd_reg_takes_event). Anything else it lets through goes out as an
 * error, since no handler on this direction can consume it.
 *
 * That keeps an error-only registration right on both counts. It has on_error
 * set and no handler for its own direction, the caller's way of saying "tell
 * me when this fd dies, don't read it", and it sees a plain close() by the
 * peer as EPOLLIN | EPOLLRDHUP without EPOLLHUP (which needs both
 * directions down). Routing that to on_error is what keeps level-triggered
 * epoll from reporting the same event for ever with nothing draining the
 * socket. _fd_reg_interest_bits is the other half of the rule: it never arms
 * EPOLLIN for a registration with no reader or EPOLLOUT for one with no
 * writer, which keeps an error-only registration out of the always-writable
 * spin. So an is_in with no reader always means the peer hung up, never
 * that ordinary data arrived. */
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
  /* Check removed again here, at the moment of the actual call, because the
   * check at collection time isn't enough. That one runs under the stripe
   * lock before the item goes into items[] or job->items[], while
   * ccol_event_loop_remove() neither takes entry->dispatch_lock nor waits for
   * an already collected item to finish dispatching (see its header doc
   * comment). It defers the teardown of a callback in progress, which
   * protects ccol_event_loop's reg and entry memory, but promises nothing
   * about whether a callback that was collected but not yet called still
   * fires.
   *
   * So a concurrent ccol_event_loop_remove can complete strictly between the
   * collection and this function actually running, and its caller can then
   * go on to free whatever reg->arg points to. With num_reactor_threads == 1
   * that window is a handful of instructions with no possible thread switch,
   * since the collection and this call happen back to back in the same
   * function on the same thread; real but very narrow. With
   * num_reactor_threads > 1 it's a ctpool queue wait of any length, wide
   * enough for a real use-after-free that valgrind catches: the teardown in
   * tests/chttpserver/tests_mem_mgmt.c races a freshly accepted connection's
   * first readable dispatch against __chttpsvr_destroy's
   * _close_all_idle_connections step.
   *
   * chttpserver's conn->reg lifetime rule is to call ccol_event_loop_remove
   * and then free conn at once (see _conn_close in chttpserver.c). That's
   * only safe if ccol_event_loop guarantees no callback runs afterwards with
   * conn as reg->arg, and this second check of removed closes that gap for
   * both dispatch paths at once. Once removed reads true this reg never
   * touches reg->arg again. */
  if (atomic_load(&reg->removed)) return;
  /* Check paused again here for exactly the same reason as removed above.
   * ccol_event_loop_pause promises that no on_readable, on_writable or
   * on_error callback fires for reg while it's paused, and that's enforced by
   * recomputing the fd's epoll interest mask. That only stops a FUTURE
   * epoll_wait from reporting readiness for this reg and does nothing about
   * an event that was collected into a job before the pause() call took
   * effect.
   *
   * With num_reactor_threads == 1 that window is tiny, since collection and
   * this call happen back to back with no lock released in between. With
   * num_reactor_threads > 1 it's a ctpool queue wait of any length, wide
   * enough to show up in practice under valgrind, as the
   * ccol_event_loop.pause_write_direction test shows: a pipe's write end is
   * writable from the instant it exists, so the poller can collect it and
   * submit a dispatch job before the very next line of the test calls
   * ccol_event_loop_pause. Skipping here as well, not only at collection
   * time, closes that window the same way the removed check above does. */
  if (atomic_load(&reg->paused)) return;
  if (item->is_error) {
    if (reg->handlers.on_error)
      reg->handlers.on_error(loop, reg->self_handle, &item->sel_snapshot,
                             reg->arg);
  } else if (item->is_readable) {
    /* Only readiness is reported, for every selectable type; the reactor
     * never does the receive itself, the callback does (see the
     * documentation of ccol_event_readable_fn). */
    if (reg->handlers.on_readable)
      reg->handlers.on_readable(loop, reg->self_handle, &item->sel_snapshot,
                                reg->arg);
  } else if (item->is_writable) {
    if (reg->handlers.on_writable)
      reg->handlers.on_writable(loop, reg->self_handle, &item->sel_snapshot,
                                reg->arg);
  }
}

/* Drops reg's refcount once its callback (if any) has returned, and defers
 * reg for freeing when that was the last reference and reg has been removed;
 * the comment of _ccol_event_loop_defer_reg_free explains why a synchronous
 * free here would be wrong. refcount is _Atomic and
 * _ccol_event_loop_defer_reg_free is lock-free, so this needs no lock at all,
 * not even a stripe lock, since nothing here touches entry state.
 *
 * It pings reclaim_wake_efd for the same reason _ccol_event_loop_remove_finish
 * does. With num_reactor_threads > 1, this function's call site in
 * _ccol_event_loop_dispatch_job_fn runs on a ctpool WORKER thread, not on
 * poller_thread, and THIS call rather than ccol_event_loop_remove's
 * decrement may be the one that really defers reg. poller_thread has no
 * other way to find that out and could stay blocked in its epoll_wait(-1)
 * indefinitely with nothing to wake it, exactly the unbounded-latency gap
 * reclaim_wake_efd exists to close.
 *
 * A ping from the other two call sites, the num_reactor_threads == 1
 * dispatch and the unwind of a failed ctpool_submit in
 * _ccol_event_loop_poller_collect, is harmless: both run on poller_thread
 * itself and reach their next reclaim point anyway, so the redundant
 * self-ping costs one cheap eventfd write that gets drained on the very next
 * iteration.
 *
 * Nothing may be read off reg between the defer call and the ping. Unlike
 * _ccol_event_loop_remove_finish's caller, this function's caller holds no
 * resolve pin on reg, because the dispatch collection reads reg straight from
 * the entry's read_reg or write_reg slot and never through
 * _ccol_event_reg_resolve. So nothing stops poller_thread's
 * _ccol_event_loop_reclaim_pending_frees from freeing reg concurrently the
 * instant _ccol_event_loop_defer_reg_free makes it eligible (for a reg with
 * no resolve in flight, pending_resolve_count is already 0). Reading
 * reg->handlers.on_removed after that call races exactly that free, a real
 * use-after-free that ThreadSanitizer reports against tests/chttpclient's
 * async_idle_pool suite. The ping needs no such read, because it's
 * unconditional. */
static void _ccol_event_loop_release_after_dispatch(
    struct _ccol_event_loop_s *loop, ccol_event_reg_s *reg) {
  int prev = atomic_fetch_sub(&reg->refcount, 1);
  if (prev == 1 && atomic_load(&reg->removed)) {
    _ccol_event_loop_defer_reg_free(loop, reg);
    /* Unconditional, for the same reason as _ccol_event_loop_remove_finish's
     * ping (see its comment): only poller_thread ever reclaims a deferred
     * reg, and on an idle loop nothing else brings it out of epoll_wait.
     * Without the ping reg would stay pending for ever, and its bridge_efd
     * open with it. Don't read anything off reg here, since this call may
     * already have made it eligible for a concurrent free. */
    _eventfd_notify(loop->reclaim_wake_efd);
  }
}

/* Runs one queue-backed dispatch item's callback with the calling thread
 * marked as dispatching that item's queue, so destroying the queue from
 * inside the callback is reported as the caller error it is; without the
 * mark that destroy would wait on itself for ever (see
 * _queue_wait_for_dispatch_refs). The mark is cleared on the way out, so the
 * thread's later, unrelated work (the next dispatch, or a dispatch worker
 * sitting idle) stays unmarked. */
static void _queue_dispatch_run_callback(struct _ccol_event_loop_s *loop,
                                         _dispatch_item *item) {
  bool fell_back = _queue_dispatch_mark(_queue_sel_queue(&item->sel_snapshot));
  _ccol_event_loop_run_callback(loop->self_handle, item);
  _queue_dispatch_unmark(fell_back);
}

/* The dispatch path for num_reactor_threads == 1 ONLY, where collection and
 * dispatch are one synchronous call on the poller thread and no dispatch
 * pool is involved at all (see the field comments of struct
 * _ccol_event_loop_s and _ccol_event_loop_thread_fn's branch on
 * loop->dispatch_pool). With num_reactor_threads > 1 nothing ever calls it;
 * the collection half is _ccol_event_loop_poller_collect and the dispatch
 * half _ccol_event_loop_dispatch_job_fn, which runs on a ctpool worker.
 *
 * It's called once per epoll_event epoll_wait returns, on poller_thread, the
 * only thread that ever calls epoll_wait in either configuration. It
 * collects the live regs to dispatch under entry->stripe_idx's stripe lock
 * and bumps each one's refcount so nothing can free it while its callback
 * runs, then releases that lock and runs the callbacks unlocked, so a stripe
 * lock is never held across a caller's callback.
 *
 * entry->dispatch_lock is held across the WHOLE body, not just the
 * collection. With num_reactor_threads == 1 it's uncontended by
 * construction, since poller_thread is the only caller and nothing can race
 * it here, but we take it anyway so this function's logic needs no special
 * case for that configuration.
 *
 * What the lock guards against is a double dispatch: two threads each
 * dispatching the same entry while it's still ready, both getting past the
 * collection step below for the SAME reg and calling its callback at the
 * same time. That's a serialization bug rather than a memory-safety one, and
 * it can't happen inside this exact function, since only one thread ever
 * calls it whatever num_reactor_threads is. The same lock in
 * _ccol_event_loop_dispatch_job_fn, on the ctpool worker that actually runs
 * a job, closes the equivalent hazard for num_reactor_threads > 1 (see that
 * function's comment).
 *
 * dispatch_lock lives on the entry and both directions share it, so in both
 * configurations it also makes the read_reg and write_reg callbacks on the
 * same fd mutually exclusive with each other, not only with themselves. The
 * entry's dispatch_lock field comment explains why that guarantee is strict
 * on purpose. */
static void _ccol_event_loop_handle_event(struct _ccol_event_loop_s *loop,
                                          ccol_poll_event *ev) {
  event_entry *entry = (event_entry *)ev->data.ptr;

  if (entry == NULL) {
    /* The shutdown eventfd's ev.data.ptr stays NULL, since that fd only
     * exists to interrupt epoll_wait and carries nothing to dispatch. It's
     * deliberately NOT drained with a read() here, because with more than one
     * reactor thread a drain deadlocks: whichever thread handles this event
     * FIRST would reset the eventfd's counter to 0, and any other thread
     * blocked in its own epoll_wait with timeout=-1 at that moment would have
     * nothing left to see as ready, never return, and leave
     * ccol_event_loop_shutdown's join loop hanging on it for ever.
     *
     * So the counter stays above zero and is never drained, and since the
     * registration is level-triggered every epoll_wait of every thread keeps
     * seeing this fd as ready. That holds in any thread order, whether a
     * thread is already blocked or hasn't called yet, and for as long as the
     * process lives, which is exactly what we want here: a shutdown is
     * one-way, so the fd's readiness never needs revoking. Every thread hits
     * the shutting_down check at the top of its loop right afterwards, so
     * none of them spins on this indefinitely. An eventfd that nobody drains
     * can look like a missing read(), but adding one brings back exactly the
     * hang described above. */
    return;
  }

  _ccol_mutex_lock(entry->dispatch_lock);

  _dispatch_item items[2];
  size_t n_items = 0;

  /* entry->stripe_idx is written once, before the entry is published (the
   * insert into a stripe's chmap, or the epoll_ctl call), and never again,
   * so it's safe to read here before any lock is taken. */
  ccol_event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];
  _ccol_mutex_lock(stripe->lock);
  /* Under the stripe lock, an fd entry that isn't removed has an open fd,
   * because removal takes this lock and the fd is only closed after that. So
   * it's safe to ask the fd what it is here; see ccol_poll_refine. */
  if (entry->is_fd && !atomic_load(&entry->removed))
    ccol_poll_refine(
        ev, entry->fd,
        _fd_reg_interest_bits(entry->as.fd.read_reg, ccol_select_read) |
            _fd_reg_interest_bits(entry->as.fd.write_reg, ccol_select_write),
        &entry->poll_info);

  if (entry->is_fd) {
    bool is_err = (ev->events & (CCOL_POLL_ERR | CCOL_POLL_HUP)) != 0;
    bool is_in = (ev->events & (CCOL_POLL_IN | CCOL_POLL_RDHUP)) != 0;
    bool is_out = (ev->events & CCOL_POLL_OUT) != 0;

    /* These three bits become a dispatch through the shared rule (see
     * _ccol_fd_item_fill). This path and _ccol_event_loop_poller_collect
     * differ in locking and in where the work goes, never in that
     * decision. */
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
    /* A queue entry. The bridge eventfd is drained here under this stripe's
     * lock, which a concurrent ccol_event_loop_remove also takes for this
     * exact entry before touching reg->bridge_efd, so the two can never
     * read() and close() that fd at the same time. entry->as.reg can
     * legitimately be NULL here, since ccol_event_loop_remove sets it to NULL
     * under this same stripe lock before deferring the entry's free, so a
     * stale batch entry arriving here after a concurrent removal counts
     * as nothing to do and must not be dereferenced. */
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

  _ccol_mutex_unlock(stripe->lock);

  for (size_t i = 0; i < n_items; i++) {
    if (entry->is_fd) {
      _ccol_event_loop_run_callback(loop->self_handle, &items[i]);
    } else {
      _queue_dispatch_run_callback(loop, &items[i]);
      /* Only for a queue-backed entry (see the comment of
       * _ccol_event_loop_queue_cascade_notify_next), since an fd entry has
       * no waiter list to forward through. The collection's refcount bump
       * still pins items[i].reg for this call and is released strictly
       * below. */
      _ccol_event_loop_queue_cascade_notify_next(
          loop, items[i].reg, items[i].sel_snapshot.dir,
          items[i].queue_count_before, items[i].wake_hops);
      /* The cascade step is this path's last touch of the queue, so the
       * queue reference is dropped here and nothing below reads the queue
       * again; a destroy blocked on it can carry on at once. */
      _queue_dispatch_ref_release(&items[i].sel_snapshot);
    }
    _ccol_event_loop_release_after_dispatch(loop, items[i].reg);
  }

  _ccol_mutex_unlock(entry->dispatch_lock);
}

/* One collected dispatch batch for a single entry, up to 2 items, exactly
 * like _ccol_event_loop_handle_event's stack-local items[2]. It's submitted
 * as a single ctpool job, so two items run back to back on one worker under
 * one acquisition of entry->dispatch_lock. That way the > 1 path keeps
 * exactly the atomicity and ordering _ccol_event_loop_handle_event has on
 * the num_reactor_threads == 1 path, instead of introducing a new
 * interleaving nobody has thought through. The job needs to outlive the
 * poller's collection call to reach whichever ctpool worker finally runs it.
 *
 * Each entry owns exactly one job, as the tail of its own allocation. An
 * entry never has more than one job queued or running, because
 * _ccol_event_loop_poller_collect drops every event for an entry whose
 * refcount is above zero, so the poller allocates nothing for a dispatch and
 * no allocation failure can drop one. The entry owns the job until
 * entry->refcount is released at the end of
 * _ccol_event_loop_dispatch_job_fn, and nothing reads it after that. */
typedef struct _dispatch_job {
  struct _ccol_event_loop_s *loop;
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
    const struct _ccol_event_loop_s *loop) {
  return loop->dispatch_pool ? sizeof(_event_entry_with_job)
                             : sizeof(event_entry);
}

/* entry's job storage, valid only for an entry of a loop with a
 * dispatch_pool (see _ccol_event_loop_entry_alloc_size). */
static inline _dispatch_job *_ccol_event_loop_entry_job(event_entry *entry) {
  return &((_event_entry_with_job *)entry)->job;
}

/* Forward declaration, since _ccol_event_loop_poller_collect below submits
 * jobs to this function with ctpool_submit before its definition appears
 * later in this file. */
static void _ccol_event_loop_dispatch_job_fn(void *arg);

/* A process-wide thread-local key, created on first use. On a dispatch_pool
 * worker it holds (as an opaque `void *`) the ccol_event_loop whose dispatch
 * job is running on the calling thread right now, or NULL when none is. On a
 * loop's poller thread it holds that loop for the thread's whole life, and
 * _ccol_event_loop_thread_fn clears it before the thread ends. Every other
 * thread holds NULL. Only the self-call guards of ccol_event_loop_shutdown
 * and ccol_event_loop_destroy read it; see the comment of the first one.
 *
 * It covers the whole process on purpose rather than one loop. Each ctpool
 * worker thread is owned privately by one ccol_event_loop instance, through
 * its dispatch_pool, for the thread's whole life, because a ctpool always
 * spawns and owns its own dedicated OS threads and never shares them between
 * pools. So one process-wide key correctly answers "which loop's job is
 * running on me right now" for every ccol_event_loop instance in the
 * process, several independent ones included, with no per-loop key
 * lifecycle to manage.
 *
 * Destroying a single loop never tears the key down. live and the deletion
 * follow the queue dispatch marker key exactly (see the comment of
 * queue_dispatch_marker_bundle). ccol_event_loop_create_mp refuses to build a
 * loop while live is false, and every other use of the key comes from a live
 * loop. */
static struct {
  _ccol_thread_ls_key_t key;
  _ccol_once_flag_t once;
  atomic_bool live;
} ccol_event_loop_job_key_bundle = {.once = _CCOL_ONCE_INIT};

static void _ccol_event_loop_init_job_key(void) {
  if (_ccol_thread_ls_key_create(ccol_event_loop_job_key_bundle.key, NULL) == 0)
    atomic_store(&ccol_event_loop_job_key_bundle.live, true);
}

/* The mark of a poller or dispatch worker whose key couldn't hold it; see
 * _queue_dispatch_mark_fallback. */
static __thread const void *_ccol_event_loop_job_mark_fallback;

/* Marks the calling thread as loop's poller or one of its dispatch workers.
 * Returns true when the key couldn't hold the mark and the thread-local copy
 * holds it instead, and the caller passes that on to
 * _ccol_event_loop_job_unmark. The key exists, since
 * ccol_event_loop_create_mp refuses to build a loop otherwise. */
static bool _ccol_event_loop_job_mark(struct _ccol_event_loop_s *loop) {
  if (_cthreadcomm_mark_set(ccol_event_loop_job_key_bundle.key, loop) == 0)
    return false;
  _ccol_event_loop_job_mark_fallback = loop;
  return true;
}

static void _ccol_event_loop_job_unmark(bool fell_back) {
  if (fell_back)
    _ccol_event_loop_job_mark_fallback = NULL;
  else
    (void)_ccol_thread_ls_set(ccol_event_loop_job_key_bundle.key, NULL);
}

/* True when the calling thread is loop's poller or is running one of loop's
 * dispatch jobs; ccol_event_loop_shutdown's and ccol_event_loop_destroy's
 * self-call guards ask this. */
static bool _ccol_event_loop_job_marked(const struct _ccol_event_loop_s *loop) {
  _ccol_call_once(ccol_event_loop_job_key_bundle.once,
                  _ccol_event_loop_init_job_key);
  return _ccol_thread_ls_get(ccol_event_loop_job_key_bundle.key) ==
             (const void *)loop ||
         _ccol_event_loop_job_mark_fallback == (const void *)loop;
}

/* The poller path for num_reactor_threads > 1. It collects entry's dispatch
 * items under that entry's stripe lock only and takes no
 * entry->dispatch_lock; the design comment above
 * _ccol_event_loop_handle_event explains why the num_reactor_threads == 1
 * path needs that lock there. This path can't race itself that way, since
 * there's exactly one poller_thread and collections are always serial, and a
 * ctpool worker only takes entry->dispatch_lock later, when it actually runs
 * the job. Same as ccol_event_loop_modify, which never takes it either (see
 * its comment).
 *
 * It then bumps entry's refcount, which protects the job it's about to
 * submit from a concurrent ccol_event_loop_remove or reclaim (see the
 * comment of event_entry.refcount), and finally submits entry's own job to
 * dispatch_pool for a worker to run.
 *
 * Returns true when the submit failed; the event is then dropped, the entry
 * re-armed, and the caller backs off before its next epoll_wait (see
 * _ccol_event_loop_dispatch_backoff).
 *
 * It is deliberately almost identical to _ccol_event_loop_handle_event's
 * collection logic while sharing no code with it, because the
 * num_reactor_threads == 1 path must stay free of every dispatch-pool
 * concern, a hard requirement (see the field comment of struct
 * _ccol_event_loop_s). So this file carries two distinct dispatch code paths
 * instead of one unified one, a deliberate trade of complexity for
 * correctness and performance. It isn't free, and it is a real, ongoing
 * maintenance surface.
 *
 * Unlike _ccol_event_loop_handle_event it does NOT drain a queue
 * selectable's bridge eventfd; the comment of
 * _ccol_event_loop_dispatch_job_fn explains why that drain has to wait until
 * the job actually runs on a worker. */
static bool _ccol_event_loop_poller_collect(struct _ccol_event_loop_s *loop,
                                            ccol_poll_event *ev) {
  event_entry *entry = (event_entry *)ev->data.ptr;
  if (entry == NULL)
    return false; /* The shutdown sentinel. See the identical comment in
                     _ccol_event_loop_handle_event. */

  ccol_event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];
  _ccol_mutex_lock(stripe->lock);
  /* Under the stripe lock, an fd entry that isn't removed has an open fd,
   * because removal takes this lock and the fd is only closed after that. So
   * it's safe to ask the fd what it is here; see ccol_poll_refine. */
  if (entry->is_fd && !atomic_load(&entry->removed))
    ccol_poll_refine(
        ev, entry->fd,
        _fd_reg_interest_bits(entry->as.fd.read_reg, ccol_select_read) |
            _fd_reg_interest_bits(entry->as.fd.write_reg, ccol_select_write),
        &entry->poll_info);

  /* A job for this entry is already queued or running, so this event is
   * dropped entirely instead of submitting a second, concurrent job. Without
   * this check there's a real use-after-free that valgrind catches.
   *
   * Application code may call ccol_event_loop_modify from WITHIN an
   * in-flight callback, and it does: chttpclient.c's TLS and request state
   * machine flips WANT_READ and WANT_WRITE as it goes. ccol_event_loop_modify
   * re-arms EPOLLONESHOT through this same shared helper (see
   * _ccol_event_loop_rearm_entry_locked) as a real post-dispatch re-arm. The
   * fd may already be ready again at that moment, since on loopback a real
   * response can arrive within microseconds, so the poller could collect and
   * submit a SECOND job for this entry while the FIRST job's callback is
   * running, and that first callback is about to free application state the
   * second job's callback then reads. The second callback is correctly
   * serialised to run strictly AFTER the first, but the first only releases
   * dispatch_lock once that state is already freed.
   *
   * entry->refcount answers exactly one question: is there already a job for
   * this entry that hasn't reached that point yet? It's bumped below and only
   * released once both steps of the in-flight job are done, the
   * dispatch_lock-protected callback and the post-dispatch re-arm (see
   * _ccol_event_loop_dispatch_job_fn). The check reads it under the same
   * stripe lock that guards the matching decrement, so it never looks
   * inconsistent.
   *
   * Dropping the event is safe because the in-flight job's eventual re-arm,
   * not this event, reflects the state correctly as of the moment that job
   * really finishes, and any genuinely new readiness after that comes back
   * fresh. */
  if (atomic_load(&entry->refcount) > 0) {
    _ccol_mutex_unlock(stripe->lock);
    return false;
  }

  /* refcount is 0 under the stripe lock, so none of this entry's jobs is
   * queued or running and its job storage is free to fill. */
  _dispatch_job *job = _ccol_event_loop_entry_job(entry);
  job->loop = loop;
  job->entry = entry;
  job->n_items = 0;

  if (entry->is_fd) {
    bool is_err = (ev->events & (CCOL_POLL_ERR | CCOL_POLL_HUP)) != 0;
    bool is_in = (ev->events & (CCOL_POLL_IN | CCOL_POLL_RDHUP)) != 0;
    bool is_out = (ev->events & CCOL_POLL_OUT) != 0;

    /* Exactly the same EPOLLIN versus EPOLLERR and EPOLLHUP priority as
     * _ccol_event_loop_handle_event (that function's comment gives the
     * reasoning). Readable wins only when a real reader is registered to take
     * the data, and every other condition the collection guard lets through
     * goes out as an error instead of being dropped and left for
     * level-triggered epoll to report again for ever. */
    /* Same shared rule as _ccol_event_loop_handle_event; see
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
    /* A queue entry. entry->as.reg can legitimately be NULL here, since a
     * concurrent ccol_event_loop_remove sets it to NULL under this very
     * stripe lock before deferring the entry's free (the identical check in
     * _ccol_event_loop_handle_event explains why). Draining the bridge
     * eventfd is deliberately left to _ccol_event_loop_dispatch_job_fn, and
     * no reference to the queue is claimed here;
     * _queue_dispatch_ref_acquire explains why only the job itself, once it
     * runs, may claim one. */
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
    /* Nothing live to dispatch (every candidate reg was removed by the time
     * the collection ran, say), so no entry refcount was bumped and there's
     * nothing to submit. The entry stays un-rearmed when EPOLLONESHOT already
     * fired for it, and that's right: with no live reg left nothing must ever
     * be notified for it again, and from here on ccol_event_loop_remove's
     * EPOLL_CTL_DEL and EPOLL_CTL_MOD calls own the entry's real interest
     * state. */
    _ccol_mutex_unlock(stripe->lock);
    return false;
  }

  atomic_fetch_add(&entry->refcount, 1);
  _ccol_mutex_unlock(stripe->lock);

  ccol_retval_t rv = ctpool_submit(loop->dispatch_pool,
                                   _ccol_event_loop_dispatch_job_fn, job, NULL);
  if (rv != ccol_success) {
    /* On this module's always-unbounded queue ctpool_submit can only really
     * fail with ccol_not_enough_memory, when the pool couldn't allocate a
     * task node. We unwind exactly what was bumped above, drop the dispatch,
     * take the stripe lock again (it was released before the submit) and
     * re-arm. The kernel disarms an EPOLLONESHOT registration the moment it
     * reports an event, whatever userspace then does with it, so without the
     * re-arm the entry would never be reported again, not just later. The
     * refcount release and the re-arm happen together under the lock, paired
     * the same way and for the same reason as in
     * _ccol_event_loop_dispatch_job_fn (see its comment).
     *
     * A level-triggered fd that is still ready reports again on the very next
     * epoll_wait. Returning true makes the poller back off before that wait,
     * so a persistent allocation failure can't make it spin. */
    for (size_t i = 0; i < job->n_items; i++)
      _ccol_event_loop_release_after_dispatch(loop, job->items[i].reg);
    _ccol_mutex_lock(stripe->lock);
    _ccol_event_loop_rearm_entry_locked(loop, entry);
    atomic_fetch_sub(&entry->refcount, 1);
    _ccol_mutex_unlock(stripe->lock);
    return true;
  }
  return false;
}

/* The ctpool task function, num_reactor_threads > 1 only. It runs on a
 * dispatch_pool worker thread, one job at a time, and locks
 * entry->dispatch_lock around the actual callback calls; the poller never
 * holds that lock (see the comment of _ccol_event_loop_poller_collect). For
 * a queue item it also claims, here and only here, the queue reference that
 * a destroy of the queue waits for (see _queue_dispatch_ref_acquire).
 *
 * A queue item's bridge eventfd is drained here and deliberately not at
 * collection time, so a failed submit in _ccol_event_loop_poller_collect
 * never silently loses a queue notification. reg's refcount, bumped at
 * collection time, keeps bridge_efd alive and valid for this read whatever a
 * concurrent ccol_event_loop_remove does. Then the EPOLLONESHOT interest is
 * re-armed through the shared helper, before entry's refcount is released.
 *
 * That order matters: the re-arm happens strictly BEFORE entry's refcount is
 * released, for two reasons. The re-arm step itself needs entry to be a
 * valid, live struct, and _ccol_event_loop_reclaim_pending_frees, the only
 * place that ever really frees an entry, checks refcount == 0, so releasing
 * first would let a concurrent reclaim free entry from under a re-arm that's
 * in flight on this thread. */
static void _ccol_event_loop_dispatch_job_fn(void *arg) {
  _dispatch_job *job = (_dispatch_job *)arg;
  struct _ccol_event_loop_s *loop = job->loop;
  event_entry *entry = job->entry;

  _ccol_call_once(ccol_event_loop_job_key_bundle.once,
                  _ccol_event_loop_init_job_key);
  bool fell_back = _ccol_event_loop_job_mark(loop);

  ccol_event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];

  _ccol_mutex_lock(entry->dispatch_lock);
  for (size_t i = 0; i < job->n_items; i++) {
    _dispatch_item *item = &job->items[i];
    if (entry->is_fd) {
      _ccol_event_loop_run_callback(loop->self_handle, item);
    } else {
      /* The queue reference is claimed here, when the job runs, under the
       * stripe lock that also orders it against ccol_event_loop_remove's
       * removed store (see _queue_dispatch_ref_acquire). A removed
       * registration claims nothing and doesn't touch the queue at all: the
       * application may already have destroyed that queue, which a destroy
       * waiting on this queued job could never have allowed. The removal also
       * took the bridge eventfd out of epoll, so there's nothing left to
       * drain. */
      _ccol_mutex_lock(stripe->lock);
      bool live = !atomic_load(&item->reg->removed);
      if (live) {
        _queue_dispatch_ref_acquire(&item->sel_snapshot);
        _queue_reg_consume_wake(item->reg, &item->queue_count_before,
                                &item->wake_hops);
      }
      _ccol_mutex_unlock(stripe->lock);
      if (live) {
        _queue_dispatch_run_callback(loop, item);
        /* The identical calls in _ccol_event_loop_handle_event explain why
         * both of these are for a queue-backed entry only and why the cascade
         * has to run before the pin release below: the cascade is this path's
         * last touch of the queue, and the queue reference only comes off once
         * it returns. */
        _ccol_event_loop_queue_cascade_notify_next(
            loop, item->reg, item->sel_snapshot.dir, item->queue_count_before,
            item->wake_hops);
        _queue_dispatch_ref_release(&item->sel_snapshot);
      }
    }
    _ccol_event_loop_release_after_dispatch(loop, item->reg);
  }
  _ccol_mutex_unlock(entry->dispatch_lock);

  /* The re-arm and the refcount release happen together under the same
   * stripe lock, because _ccol_event_loop_poller_collect's "a job is already
   * in flight for this entry" check reads entry->refcount under this very
   * lock and must never see "already re-armed" while the refcount is still
   * above zero. In such a window the re-arm is done but the decrement isn't
   * yet visible to a poller collecting at the same moment, so the poller
   * would wrongly treat a really new readiness event that arrived after the
   * re-arm as "still in flight" and silently drop it, while EPOLLONESHOT has
   * already used up that notification and nothing is left to re-arm it.
   * That's a real failure, just quieter than the use-after-free this refcount
   * check exists to prevent. */
  _ccol_mutex_lock(stripe->lock);
  _ccol_event_loop_rearm_entry_locked(loop, entry);
  atomic_fetch_sub(&entry->refcount, 1);
  _ccol_mutex_unlock(stripe->lock);
  /* entry owns job. Once the release above has happened the poller may fill
   * job for a new dispatch and a reclaim may free entry together with it, so
   * nothing below touches either. */

  _ccol_event_loop_job_unmark(fell_back);
}

/* Retry interval for the next epoll_wait whenever
 * _ccol_event_loop_reclaim_pending_frees leaves something pending. It's a
 * correctness backstop (that function's comment explains why a ping alone
 * isn't reliable), not a latency target this module is tuned around. 50ms is
 * comfortably below the timeout budget of every existing on_removed latency
 * test, and long enough that a registration stuck for a long time, because
 * of an application bug elsewhere rather than in this module, causes no
 * meaningful busy-polling. The cost is one return from epoll_wait plus one
 * lock-free list scan every 50ms, paid only while at least one entry or reg
 * is pending. */
#define EVENT_LOOP_RECLAIM_RETRY_MS 50

/* The first and the longest pause of _ccol_event_loop_dispatch_backoff. */
#define EVENT_LOOP_DISPATCH_BACKOFF_MIN_MS 1
#define EVENT_LOOP_DISPATCH_BACKOFF_MAX_MS 64

/* Pauses poller_thread after a batch in which a dispatch couldn't be
 * submitted. That dispatch's entry is re-armed, and a level-triggered fd that
 * is still ready reports again on the very next epoll_wait, so without a
 * pause a persistent allocation failure would turn the poller into a busy
 * loop burning a whole CPU without making progress. The pause doubles with
 * each consecutive failed batch, from EVENT_LOOP_DISPATCH_BACKOFF_MIN_MS up to
 * EVENT_LOOP_DISPATCH_BACKOFF_MAX_MS, and a batch without a failure resets it.
 * It waits on shutdown_efd, which a shutdown writes and nothing drains, so a
 * shutdown ends the pause at once. */
static __attribute__((noinline, cold)) void _ccol_event_loop_dispatch_backoff(
    struct _ccol_event_loop_s *loop, int *backoff_ms) {
  *backoff_ms =
      *backoff_ms == 0 ? EVENT_LOOP_DISPATCH_BACKOFF_MIN_MS : *backoff_ms * 2;
  if (*backoff_ms > EVENT_LOOP_DISPATCH_BACKOFF_MAX_MS)
    *backoff_ms = EVENT_LOOP_DISPATCH_BACKOFF_MAX_MS;
  struct pollfd pfd = {.fd = loop->shutdown_efd, .events = POLLIN};
  (void)poll(&pfd, 1, *backoff_ms);
}

static void *_ccol_event_loop_thread_fn(void *arg) {
  struct _ccol_event_loop_s *loop = (struct _ccol_event_loop_s *)arg;
  ccol_poll_event *events = loop->poller_events;
  int backoff_ms = 0;

  /* The mark identifying this thread as loop's poller, for the self-call
   * guards of ccol_event_loop_shutdown and ccol_event_loop_destroy. It stays
   * for the thread's whole life, since callbacks and on_removed
   * notifications run here. The key is live, because
   * ccol_event_loop_create_mp won't build a loop otherwise. */
  bool fell_back = _ccol_event_loop_job_mark(loop);

  for (;;) {
    if (atomic_load(&loop->shutting_down)) break;

    /* The point between two batches: by now poller_thread has fully walked
     * its previous batch, if it had one. We publish that by advancing
     * poller_batch_gen and then try to reclaim the deferred entries and regs
     * that the advance proves safe to free. The big comment above
     * _ccol_event_loop_reclaim_pending_frees describes the design. */
    atomic_fetch_add(&loop->poller_batch_gen, (uint64_t)1);
    bool reclaim_still_pending = _ccol_event_loop_reclaim_pending_frees(loop);

    /* EVENT_LOOP_RECLAIM_RETRY_MS instead of -1 whenever anything is pending,
     * for the reason in _ccol_event_loop_reclaim_pending_frees's comment.
     * This bounded retry, not a perfectly timed ping, is what guarantees
     * forward progress here. */
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
      /* reclaim_wake_efd's sentinel (see its field comment), with nothing to
       * dispatch. We only drain it so another ping can wake the poller later.
       * The reclaim at the top of this loop already ran unconditionally
       * before this epoll_wait, and it picks up whatever this ping was about
       * on the NEXT iteration. This check comes before the shutdown-sentinel
       * branch inside _ccol_event_loop_handle_event and
       * _ccol_event_loop_poller_collect, because that branch tests
       * ev.data.ptr == NULL and this fd's ev.data.ptr is never NULL. */
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

  /* Remove the mark before the thread ends so no later thread can inherit
   * it. glibc gives a joined thread's descriptor, and with it its thread ID,
   * to the next thread that starts, which is why the guards never compare
   * _ccol_get_thread_id() with poller_thread. */
  _ccol_event_loop_job_unmark(fell_back);
  return NULL;
}

/* Destroys the first `created` stripes of loop->stripes (each with a mutex
 * and a chmap) and then frees the array. It uses the raw mmgmt_procs
 * parameter rather than loop->m_procs, like every other _ccol_mem_free call
 * in ccol_event_loop_create_mp, some of which run before loop->m_procs is
 * even filled in. It's only used to roll back a failure at creation time;
 * __ccol_event_loop_destroy's stripe teardown does more (it frees the live
 * entries first) and isn't built on it. */
static void _destroy_stripes(struct _ccol_event_loop_s *loop,
                             ccol_memmgmt_procs_t *mmgmt_procs,
                             size_t created) {
  for (size_t i = 0; i < created; i++) {
    chmap_destroy(loop->stripes[i].fd_index);
    _ccol_mutex_destroy(loop->stripes[i].lock);
  }
  _ccol_mem_free(mmgmt_procs, loop->stripes);
}

/* Forward declaration; the definition comes after ccol_event_loop_shutdown
 * and _ccol_event_loop_shutdown_internal, but ccol_event_loop_create_mp's
 * rollback after a failed slot acquire needs it here. */
static void _ccol_event_loop_teardown_raw(struct _ccol_event_loop_s *loop);

ccol_event_loop ccol_event_loop_create_mp(size_t max_events_per_wait,
                                          size_t num_lock_stripes,
                                          size_t num_reactor_threads,
                                          ccol_memmgmt_procs_t *mmgmt_procs,
                                          char **err_str) {
  if (max_events_per_wait == 0) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("max_events_per_wait must be positive");
    return CCOL_EVENT_LOOP_INVALID;
  }
  /* max_events_per_wait gets narrowed to a plain `int` for epoll_wait's
   * maxevents parameter in _ccol_event_loop_thread_fn, and it also sizes the
   * poller thread's events buffer below, an allocation of
   * max_events_per_wait * sizeof(struct epoll_event).
   *
   * An absurdly large value breaks one of two ways. The (int) cast can give
   * a zero or negative maxevents, so epoll_wait fails with EINVAL, or
   * max_events_per_wait * sizeof(struct epoll_event) can overflow size_t on
   * an ILP32 platform, handing a too-small buffer to a maxevents the kernel
   * thinks is much bigger. _ccol_event_loop_thread_fn treats a failed
   * epoll_wait as a permanent exit of the poller thread, on its very first
   * iteration, and the constructor's caller never hears about it, holding a
   * valid-looking handle that never dispatches anything.
   *
   * So such a value is rejected here, in line with the SIZE_MAX / sizeof(x)
   * overflow-guard convention verify_circular_queue_create_inputs uses for
   * the same kind of hazard. */
  if (max_events_per_wait > (size_t)INT_MAX) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("max_events_per_wait must not exceed INT_MAX");
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (max_events_per_wait > SIZE_MAX / sizeof(ccol_poll_event)) {
    if (err_str)
      *err_str = _CCOL_ERR_STR(
          "max_events_per_wait is too large: max_events_per_wait * "
          "sizeof(ccol_poll_event) would overflow size_t");
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (num_lock_stripes == 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("num_lock_stripes must be positive");
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (num_reactor_threads == 0) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("num_reactor_threads must be positive");
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (!_ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return CCOL_EVENT_LOOP_INVALID;
  }

  /* A loop's dispatch sets two of this module's thread-local keys. A key
   * that couldn't be created names no key of ours, so a loop that would set
   * it is refused (see queue_dispatch_marker_bundle). */
  _ccol_call_once(ccol_event_loop_job_key_bundle.once,
                  _ccol_event_loop_init_job_key);
  if (!atomic_load(&ccol_event_loop_job_key_bundle.live) ||
      !_queue_dispatch_marker_available()) {
    if (err_str)
      *err_str = _CCOL_ERR_STR(
          "failed to create a thread-local key of ccol_event_loop");
    return CCOL_EVENT_LOOP_INVALID;
  }

  struct _ccol_event_loop_s *loop =
      (struct _ccol_event_loop_s *)_ccol_mem_alloc(
          mmgmt_procs, sizeof(struct _ccol_event_loop_s));
  if (!loop) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("Failed to allocate memory for ccol_event_loop");
    return CCOL_EVENT_LOOP_INVALID;
  }

  if (!_ccol_populate_mem_mgmt_procs(loop, mmgmt_procs, err_str)) {
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  loop->epfd = ccol_poll_create();
  if (loop->epfd < 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("epoll_create1 failed");
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  loop->shutdown_efd = ccol_wakefd_create();
  if (loop->shutdown_efd < 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("eventfd failed");
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  ccol_poll_event ev;
  ev.data.ptr = NULL;
  ev.events = CCOL_POLL_IN;
  if (ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_ADD, loop->shutdown_efd, &ev,
                    NULL) < 0) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("epoll_ctl failed registering shutdown eventfd");
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  loop->reclaim_wake_efd = ccol_wakefd_create();
  if (loop->reclaim_wake_efd < 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("eventfd failed");
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  ev.data.ptr = &loop->reclaim_wake_efd;
  ev.events = CCOL_POLL_IN;
  if (ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_ADD, loop->reclaim_wake_efd, &ev,
                    NULL) < 0) {
    if (err_str)
      *err_str =
          _CCOL_ERR_STR("epoll_ctl failed registering reclaim-wake eventfd");
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  if (_ccol_mutex_init(loop->shutdown_lock) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("Failed to initialize shutdown_lock");
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (_ccol_cond_var_init(loop->joined_cv) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("Failed to initialize joined_cv");
    _ccol_mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }
  if (_ccol_rw_lock_init(loop->reg_slot_rwlock) != 0) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("Failed to initialize reg_slot_rwlock");
    _ccol_cond_var_destroy(loop->joined_cv);
    _ccol_mutex_destroy(loop->shutdown_lock);
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
    if (err_str) *err_str = _CCOL_ERR_STR("Failed to allocate reg slot array");
    _ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    _ccol_cond_var_destroy(loop->joined_cv);
    _ccol_mutex_destroy(loop->shutdown_lock);
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
      *err_str = _CCOL_ERR_STR("Failed to allocate reg free-index array");
    __cvector_destroy(loop->reg_slots);
    _ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    _ccol_cond_var_destroy(loop->joined_cv);
    _ccol_mutex_destroy(loop->shutdown_lock);
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
  /* This must be initialised strictly before poller_thread is created
   * below. pending_resolve_count and foreign_since_fork are initialised
   * further down, after _ccol_thread_create, which is safe because
   * poller_thread never touches either, but poller_thread DOES write this
   * field on every single loop iteration, so an atomic_init once that thread
   * may already be running would itself be a data race. */
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
      *err_str = _CCOL_ERR_STR("Failed to allocate lock stripe array");
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    _ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    _ccol_cond_var_destroy(loop->joined_cv);
    _ccol_mutex_destroy(loop->shutdown_lock);
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
                              : _CCOL_ERR_STR("Failed to create fd registry");
      _destroy_stripes(loop, mmgmt_procs, stripes_created);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      _ccol_rw_lock_destroy(loop->reg_slot_rwlock);
      _ccol_cond_var_destroy(loop->joined_cv);
      _ccol_mutex_destroy(loop->shutdown_lock);
      close(loop->reclaim_wake_efd);
      close(loop->shutdown_efd);
      close(loop->epfd);
      _ccol_mem_free(mmgmt_procs, loop->m_procs);
      _ccol_mem_free(mmgmt_procs, loop);
      return CCOL_EVENT_LOOP_INVALID;
    }
    if (_ccol_mutex_init(loop->stripes[stripes_created].lock) != 0) {
      if (err_str) *err_str = _CCOL_ERR_STR("Failed to initialize stripe lock");
      /* The _destroy_stripes(loop, mmgmt_procs, stripes_created) call below
       * only destroys stripes [0, stripes_created), while the current stripe
       * already has an fd_index and no validly initialised .lock, so that
       * fd_index is torn down explicitly first. The sibling !fd_index
       * failure block above handles its own current stripe exactly the same
       * way. */
      chmap_destroy(loop->stripes[stripes_created].fd_index);
      _destroy_stripes(loop, mmgmt_procs, stripes_created);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      _ccol_rw_lock_destroy(loop->reg_slot_rwlock);
      _ccol_cond_var_destroy(loop->joined_cv);
      _ccol_mutex_destroy(loop->shutdown_lock);
      close(loop->reclaim_wake_efd);
      close(loop->shutdown_efd);
      close(loop->epfd);
      _ccol_mem_free(mmgmt_procs, loop->m_procs);
      _ccol_mem_free(mmgmt_procs, loop);
      return CCOL_EVENT_LOOP_INVALID;
    }
    loop->stripes[stripes_created].queue_regs_head = NULL;
  }

  /* poller_thread's epoll_wait buffer. It's allocated here, before the thread
   * starts, so the thread has no step that can fail, and any handle this
   * function returns always names a loop that dispatches. The size can't
   * overflow, thanks to the max_events_per_wait checks at the top of this
   * function. */
  loop->poller_events = _ccol_mem_alloc(
      mmgmt_procs, max_events_per_wait * sizeof(ccol_poll_event));
  if (!loop->poller_events) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("Failed to allocate the epoll_wait buffer");
    _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    _ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    _ccol_cond_var_destroy(loop->joined_cv);
    _ccol_mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _ccol_mem_free(mmgmt_procs, loop->m_procs);
    _ccol_mem_free(mmgmt_procs, loop);
    return CCOL_EVENT_LOOP_INVALID;
  }

  /* dispatch_pool only exists for num_reactor_threads > 1 (see the field
   * comment of struct _ccol_event_loop_s) and has num_reactor_threads - 1
   * threads, so the total OS thread count stays exactly num_reactor_threads,
   * 1 for poller_thread plus this pool, and the parameter keeps its
   * documented meaning for resource usage. The pool's queue is unbounded
   * (queue_capacity is 0) because the poller must never block on a
   * submission; see _ccol_event_loop_poller_collect's comment on how
   * ctpool_submit failures are handled. The pool is created before
   * poller_thread is spawned below, so dispatch_pool is always fully valid
   * before any event can reach it. */
  if (num_reactor_threads > 1) {
    char *pool_err = NULL;
    loop->dispatch_pool =
        ctpool_create_mp(num_reactor_threads - 1, 0, mmgmt_procs, &pool_err);
    if (!loop->dispatch_pool) {
      if (err_str)
        *err_str = pool_err ? pool_err
                            : _CCOL_ERR_STR("Failed to create dispatch pool");
      _ccol_mem_free(mmgmt_procs, loop->poller_events);
      _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      _ccol_rw_lock_destroy(loop->reg_slot_rwlock);
      _ccol_cond_var_destroy(loop->joined_cv);
      _ccol_mutex_destroy(loop->shutdown_lock);
      close(loop->reclaim_wake_efd);
      close(loop->shutdown_efd);
      close(loop->epfd);
      _ccol_mem_free(mmgmt_procs, loop->m_procs);
      _ccol_mem_free(mmgmt_procs, loop);
      return CCOL_EVENT_LOOP_INVALID;
    }
  }

  if (_ccol_thread_create(loop->poller_thread, _ccol_event_loop_thread_fn,
                          loop) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("pthread_create failed");
    if (loop->dispatch_pool) __ctpool_destroy(loop->dispatch_pool);
    _ccol_mem_free(mmgmt_procs, loop->poller_events);
    _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    _ccol_rw_lock_destroy(loop->reg_slot_rwlock);
    _ccol_cond_var_destroy(loop->joined_cv);
    _ccol_mutex_destroy(loop->shutdown_lock);
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

  /* The slot acquire is the LITERAL LAST step, after the poller thread has
   * started successfully and, if configured, dispatch_pool too. That mirrors
   * the chttpcli and chttpsvr constructors exactly, so no handle reaches any
   * caller until this function is about to return success.
   *
   * A failure here must NOT take the path of the ordinary allocation
   * failures above, because the poller thread is already running, maybe
   * with a dispatch_pool that has its own workers, and the rollback needs
   * to really stop them. So we reuse _ccol_event_loop_teardown_raw, the helper
   * __ccol_event_loop_destroy uses, whose own call to
   * _ccol_event_loop_shutdown_internal does exactly that; just freeing the
   * memory under a live thread would be wrong.
   *
   * No wait on pending_resolve_count of any kind is needed: no handle has
   * reached any caller yet, so nothing can have resolved or pinned it. */
  ccol_event_loop h = _ccol_event_loop_handle_slot_acquire(loop);
  if (h == 0) {
    if (err_str)
      *err_str =
          _CCOL_ERR_STR("Failed to allocate ccol_event_loop handle slot");
    _ccol_event_loop_teardown_raw(loop);
    return CCOL_EVENT_LOOP_INVALID;
  }
  loop->self_handle = h;

  if (err_str) *err_str = NULL;
  return h;
}

/* The real shutdown body, which takes an already resolved raw pointer. Two
 * callers use it: the thin public wrapper below, after its resolve and pin,
 * and _ccol_event_loop_teardown_raw, a helper called from two places that
 * don't resolve a handle again, __ccol_event_loop_destroy and the
 * constructor's rollback after a failed slot acquire. Once a slot is marked
 * not in use, every further resolve of it fails. */
/* Unlinks the waiter node of each of loop's queue registrations from its
 * queue's waiter list once nothing in loop dispatches any more. A wake held
 * by a shut-down loop's registration would never be acted on, and every later
 * wake the queue sent it would be lost to the queue's other listeners, so
 * each node leaves the list here and a wake moves on to the next listener
 * whenever the queue is ready, just as on a remove. The lock order is the
 * stripe lock, then the queue's mutex, as in the remove. */
static void _ccol_event_loop_detach_queue_regs(
    struct _ccol_event_loop_s *loop) {
  for (size_t i = 0; i < loop->num_stripes; i++) {
    ccol_event_loop_stripe_t *stripe = &loop->stripes[i];
    _ccol_mutex_lock(stripe->lock);
    for (ccol_event_reg_s *reg = stripe->queue_regs_head; reg;
         reg = reg->loop_list_next) {
      _ccol_mutex_t *q_mtx;
      ccol_sel_waiter **q_head;
      ccol_sel_waiter **q_rotor;
      _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
      _ccol_mutex_lock(*q_mtx);
      if (!reg->waiter_detached && !atomic_load(&reg->removed)) {
        _sel_unlink_waiter_locked(&reg->waiter_node, q_head, q_rotor);
        reg->waiter_detached = true;
        (*_queue_sel_detached_count(&reg->sel))++;
        if (_queue_sel_is_ready(&reg->sel))
          notify_one_sel_waiter(q_head, q_rotor);
      }
      _ccol_mutex_unlock(*q_mtx);
    }
    _ccol_mutex_unlock(stripe->lock);
  }
}

static ccol_retval_t _ccol_event_loop_shutdown_internal(
    struct _ccol_event_loop_s *loop) {
  /* The self-call guard. Joining poller_thread below from poller_thread
   * itself deadlocks just like a bare self-pthread_join, and draining
   * dispatch_pool from one of its own workers deadlocks the same way.
   * ctpool_shutdown_drain notices when one of its own pool's workers calls
   * it and returns without a join, so the drain side is covered there; this
   * guard makes the poller-thread side, which the pool knows nothing about,
   * fail the same way, so both get one uniform answer.
   *
   * It applies to BOTH configurations alike. With num_reactor_threads == 1
   * poller_thread is the only thread and obviously the one running whatever
   * callback can call this; with num_reactor_threads > 1 the caller can be
   * any dispatch_pool worker. Guarding just one configuration would make this
   * function's behaviour on misuse depend on how many reactor threads the
   * caller configured, a worse API than either "always undefended" or
   * "always defended".
   *
   * It returns ccol_not_permitted instead of deadlocking silently. That
   * enumerator already exists and fits exactly (the operation isn't allowed
   * in the current state), so we reuse it instead of minting a new one; see
   * common.h's hard rule on enumerator numbering.
   *
   * The guard recognises the poller and the dispatch workers by the mark each
   * holds in ccol_event_loop_job_key_bundle, never by comparing
   * _ccol_get_thread_id() with poller_thread: once poller_thread is joined,
   * glibc hands its thread ID to the next thread that starts, and an
   * unrelated thread calling this after a shutdown would then be refused. The
   * mark holds the RAW pointer, a struct _ccol_event_loop_s*, not the public
   * uint64_t handle value. */
  if (_ccol_event_loop_job_marked(loop)) return ccol_not_permitted;

  _ccol_mutex_lock(loop->shutdown_lock);
  bool is_leader = !loop->shutdown_started;
  loop->shutdown_started = true;
  _ccol_mutex_unlock(loop->shutdown_lock);

  if (is_leader) {
    atomic_store(&loop->shutting_down, true);

#if CCOL_FORK_SAFETY_REQUIRED
    bool is_foreign = atomic_load(&loop->foreign_since_fork);
#else
    bool is_foreign = false;
#endif
    if (is_foreign) {
      /* This process inherited `loop` across a fork() (see that field's
       * comment). Here poller_thread is just inert copy-on-write memory that
       * never had an execution context in this process, while shutdown_efd is
       * a real kernel object shared with the parent's live, still running
       * poller thread.
       *
       * So both steps below are skipped entirely. Writing shutdown_efd here
       * would wrongly wake the PARENT's poller, a real, reproducible side
       * effect, since the eventfd object behind it is the SAME one in both
       * processes. A _ccol_thread_join(poller_thread) would be undefined
       * behaviour, which reliably SIGSEGVs in the exactly analogous case of a
       * dispatch_pool worker. cthreadpool.c's foreign_since_fork field
       * covers that half, so ctpool_shutdown_drain below is safe to call
       * unconditionally and this module needs no fork awareness of its own
       * for it. */
    } else {
      _eventfd_notify(loop->shutdown_efd);

      /* One write reliably wakes poller_thread, because the shared NULL-entry
       * handling of _ccol_event_loop_handle_event and
       * _ccol_event_loop_poller_collect deliberately never drains
       * shutdown_efd (the comment of _ccol_event_loop_handle_event describes
       * the deadlock a drain would cause). If the counter were reset to 0
       * before epoll_wait could report it, the reactor would sit in
       * epoll_wait for ever and this join would hang on it.
       *
       * So once written the counter stays above zero, and since the
       * registration is level-triggered poller_thread's epoll_wait keeps
       * seeing shutdown_efd as ready until that call really returns; the
       * loop's shutting_down check, set above before this write, then breaks
       * it out. The hang a drain causes is real, not theoretical: the reactor
       * sits in this exact epoll_wait with nothing left to report. The
       * multi-thread shutdown tests in tests/cthreadcomm/tests.c are the
       * regression coverage for this.
       *
       * poller_thread is joined strictly before dispatch_pool is drained, so
       * no further job can ever reach the pool, since only poller_thread ever
       * calls ctpool_submit. That's what makes the dispatch_pool drain below
       * safe without a separate submit-after-shutdown race to handle:
       * ctpool_shutdown_drain's documented contract says no other thread may
       * submit tasks concurrently, and at this call that holds by
       * construction. */
      _ccol_thread_join(loop->poller_thread);
    }

    /* Drain the pool rather than cancel it at once, to keep this function's
     * documented contract that no dispatch is in flight once it returns. An
     * immediate shutdown would cancel queued jobs that never started instead
     * of running them and leave their already bumped reg and entry refcounts
     * in a state nothing ever cleans up. dispatch_pool is NULL with
     * num_reactor_threads == 1, where nothing was created to drain. The call
     * is safe unconditionally, even with foreign_since_fork, because this is
     * a ctpool and cthreadpool.c's identical fork fixup makes
     * ctpool_shutdown_drain join no thread in that case (see struct
     * cthread_pool's foreign_since_fork field). */
    if (loop->dispatch_pool) {
      ctpool_shutdown_drain(loop->dispatch_pool);
    }

    /* A loop inherited across fork() shares no queue with a live poller in
     * this process, and its queue mutexes may be in any state. */
    if (!is_foreign) _ccol_event_loop_detach_queue_regs(loop);

    _ccol_mutex_lock(loop->shutdown_lock);
    loop->joined = true;
    _ccol_cond_var_broadcast(loop->joined_cv);
    _ccol_mutex_unlock(loop->shutdown_lock);
  } else {
    _ccol_mutex_lock(loop->shutdown_lock);
    while (!loop->joined) {
      _ccol_cond_var_wait(loop->joined_cv, loop->shutdown_lock);
    }
    _ccol_mutex_unlock(loop->shutdown_lock);
  }

  return ccol_success;
}

ccol_retval_t ccol_event_loop_shutdown(ccol_event_loop loop) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = _ccol_event_loop_shutdown_internal(raw);
  _ccol_event_loop_resolve_unpin(raw);
  return rv;
}

/* Shared by two callers: __ccol_event_loop_destroy, once its own poll-wait
 * for pending_resolve_count == 0 is done, and ccol_event_loop_create_mp's
 * rollback after a failed slot acquire, which waits for nothing beforehand
 * because no handle has reached any caller yet, so pending_resolve_count is
 * provably 0 already.
 *
 * It runs the shutdown if that hasn't started yet, idempotently either way,
 * through _ccol_event_loop_shutdown_internal's leader and follower protocol
 * (shutdown_lock, shutdown_started and joined_cv), and then frees every
 * remaining resource.
 *
 * It's never called on a loop whose handle a caller could still resolve.
 * Unlike ctpool's teardown helper, ccol_event_loop's wait-ordering rule says
 * to wait BEFORE this runs, not after (see the comment of
 * __ccol_event_loop_destroy), so it never has to wait on
 * pending_resolve_count itself. */
static void _ccol_event_loop_teardown_raw(struct _ccol_event_loop_s *loop) {
  _ccol_event_loop_shutdown_internal(loop);

  /* Some entries and regs were deferred during the last round of batch
   * processing, just before shutting_down was seen, and never reached a
   * reclaim point on their own defer_gen's schedule. poller_thread and every
   * dispatch_pool worker are joined by now, so none of them can touch
   * anything, and every remaining deferred item can be freed unconditionally
   * instead of leaked. The comment of _ccol_event_loop_free_all_pending
   * explains why no epoch or refcount check is needed at this point. */
  _ccol_event_loop_free_all_pending(loop);
  _ccol_mem_free(loop->m_procs, loop->poller_events);

  /* poller_thread and every dispatch_pool worker are joined, and every other
   * in-flight ccol_event_loop_shutdown caller has returned, as the leader and
   * follower join protocol above guarantees. No dispatch can be in flight
   * and no other thread can touch this loop's registry, so every remaining
   * registration in every stripe can be walked and freed without a lock.
   *
   * The stored event_entry* is read with a memcpy rather than a direct
   * pointer cast, a second line of defence matching _fd_registry_find. The
   * walk allocates nothing, so a destroy that runs out of memory still frees
   * every registration and calls every on_removed that's due. chmap_destroy
   * below only frees the map's copies of the int keys and pointer values,
   * never the event_entry structs those pointers name. */
  for (size_t i = 0; i < loop->num_stripes; i++) {
    ccol_event_loop_stripe_t *stripe = &loop->stripes[i];

    ccol_chmap_walk walk = {0};
    const void *key_bytes;
    const void *val_bytes;
    while (
        ccol_chmap_walk_next(stripe->fd_index, &walk, &key_bytes, &val_bytes)) {
      event_entry *entry;
      memcpy(&entry, val_bytes, sizeof(entry));
      if (entry->as.fd.read_reg)
        _ccol_event_reg_free(loop, entry->as.fd.read_reg);
      if (entry->as.fd.write_reg)
        _ccol_event_reg_free(loop, entry->as.fd.write_reg);
      _ccol_mutex_destroy(entry->dispatch_lock);
      _ccol_mem_free(loop->m_procs, entry);
    }
    chmap_destroy(stripe->fd_index);

    ccol_event_reg_s *reg = stripe->queue_regs_head;
    while (reg) {
      ccol_event_reg_s *next = reg->loop_list_next;
      _ccol_mutex_t *q_mtx;
      ccol_sel_waiter **q_head;
      ccol_sel_waiter **q_rotor;
      _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
      /* The queue may have other listeners, a ccol_select() caller or another
       * loop's registration. A wake this registration holds and never acted
       * on goes to the next of them, as in _ccol_event_loop_remove_unlink's
       * queue branch. */
      _ccol_mutex_lock(*q_mtx);
      if (!reg->waiter_detached) {
        _sel_unlink_waiter_locked(&reg->waiter_node, q_head, q_rotor);
        if (_queue_sel_is_ready(&reg->sel))
          notify_one_sel_waiter(q_head, q_rotor);
      } else {
        (*_queue_sel_detached_count(&reg->sel))--;
      }
      _ccol_mutex_unlock(*q_mtx);
      /* Capture the entry and free reg before the entry reg names, because
       * _ccol_event_reg_free, which frees reg, first reads reg->owning_entry
       * to decide whether this registration ever got live enough for
       * on_removed to fire. Freeing the entry first would leave that field
       * holding an indeterminate pointer value, and C11 6.2.4p2 makes even
       * evaluating such a value undefined, although we only test it for
       * non-NULL. Exactly one registration owns a queue-backed entry, so
       * nothing else can observe this order. */
      event_entry *owning = reg->owning_entry;
      _ccol_event_reg_free(loop, reg);
      _ccol_mutex_destroy(owning->dispatch_lock);
      _ccol_mem_free(loop->m_procs, owning);
      reg = next;
    }

    _ccol_mutex_destroy(stripe->lock);
  }
  _ccol_mem_free(loop->m_procs, loop->stripes);
  if (loop->dispatch_pool) __ctpool_destroy(loop->dispatch_pool);

  cvector_destroy(loop->reg_free_indices);
  cvector_destroy(loop->reg_slots);
  _ccol_rw_lock_destroy(loop->reg_slot_rwlock);
  _ccol_cond_var_destroy(loop->joined_cv);
  _ccol_mutex_destroy(loop->shutdown_lock);
  close(loop->reclaim_wake_efd);
  close(loop->shutdown_efd);
  close(loop->epfd);

  if (loop->m_procs) {
    ccol_free_t free_func = loop->m_procs->free;
    free_func(loop->m_procs);
    free_func(loop);
  } else {
    _ccol_default_mem_free(loop);
  }
}

void __ccol_event_loop_destroy(ccol_event_loop loop) {
  if (!loop) return;

  /* loop is resolved through the slot table and the teardown claimed in the
   * same critical section as the lookup, so a second destroy call on the same
   * handle value, concurrent or later, fails the lookup and never races this
   * call's teardown. The slot table's file-level comment and the comment of
   * _ccol_event_loop_resolve describe the design. A stale or already
   * destroyed handle arriving here is exactly the misuse the
   * generation-checked handle design exists to catch, and it's fatal rather
   * than a silent use-after-free or double free. */
  _ccol_call_once(ccol_event_loop_slot_table.once,
                  _ccol_event_loop_slot_table_init_globals);
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  uint32_t idx = (uint32_t)(loop >> 32);
  uint32_t gen = (uint32_t)(loop & 0xFFFFFFFFu);
  _ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  struct _ccol_event_loop_s *raw = NULL;
  if (idx < _event_loop_slot_count_locked()) {
    ccol_event_loop_slot_t *s = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
    if (s->in_use && s->generation == gen && !s->ptr->destroy_claimed)
      raw = s->ptr;
  }
  if (!raw) {
    _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
    _ccol_fatal_err(
        "ccol_event_loop_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a ccol_event_loop handle)");
  }

  /* Guard against a callback destroying its own loop, the destroy-side
   * counterpart of _ccol_event_loop_shutdown_internal's self-join guard.
   * __ccol_event_loop_destroy calls that guarded function internally, through
   * _ccol_event_loop_teardown_raw, but can't pass its ccol_not_permitted
   * return on, since this function returns void and is reached through a
   * macro.
   *
   * Without this check, a callback destroying its own loop would see the
   * internal shutdown quietly join nothing, and this function would go on to
   * free every entry, including the dispatch_lock the calling frame holds
   * locked, and the loop struct itself, all under the still running
   * _ccol_event_loop_handle_event or _ccol_event_loop_dispatch_job_fn frame
   * that called it. That's a heap use-after-free.
   *
   * The check comes here, before the teardown is claimed, so a caller misusing
   * this from inside a callback gets the same loud, detected _ccol_fatal_err()
   * as every other truly fatal misuse of this function (a stale or already
   * destroyed handle, say) instead of a silent skip followed by undefined
   * behaviour. */
  bool is_self_call = _ccol_event_loop_job_marked(raw);
  if (is_self_call) {
    _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
    _ccol_fatal_err(
        "ccol_event_loop_destroy: called from within a callback running on "
        "this "
        "loop's own reactor/dispatch thread (self-destroy hazard); defer "
        "destruction to another thread, or to after the callback returns, "
        "instead");
  }

  /* The claim makes this call the teardown's owner; a second destroy call,
   * concurrent or later, reads it and fails fatally above. The handle itself
   * deliberately still resolves, because the graceful shutdown below still
   * runs callbacks: poller_thread finishes its current batch and
   * dispatch_pool drains every job it queued. Those callbacks call
   * ccol_event_loop_remove, _pause, _resume, _modify and _add on this loop
   * just as during ccol_event_loop_shutdown. If a removal failed there, the
   * removed flag would stay clear, and a second item the same batch
   * collected for the same fd would run with an arg the first callback had
   * already freed. */
  raw->destroy_claimed = true;
  _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);

  /* _ccol_event_loop_shutdown_internal's leader and follower protocol makes
   * this safe next to a concurrent ccol_event_loop_shutdown. The self-call
   * guard above already refused a call from one of this loop's threads, so
   * it can't return ccol_not_permitted. Once it returns poller_thread is
   * joined and dispatch_pool drained, so no callback runs any more. */
  _ccol_event_loop_shutdown_internal(raw);

  /* Only now does the handle stop resolving. The slot is fetched by idx under
   * the lock, and no pointer into the slot array is kept across an unlock,
   * since a concurrent ccol_event_loop_create_mp can reallocate that array
   * with cvector_push_back. */
  _ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  ((ccol_event_loop_slot_t *)cvector_at(ccol_event_loop_slot_table.slots, idx))
      ->in_use = false;
  _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);

  /* Wait for pending_resolve_count to reach 0 BEFORE freeing anything; this
   * field's struct comment explains why waiting first is safe for
   * ccol_event_loop but not for ctpool. Every public entry point that holds a
   * pin (ccol_event_loop_add, _modify, _pause, _resume, _remove and
   * _reg_count) is a quick, bounded critical section that only takes a stripe
   * lock, and none of them blocks on the poller thread or on the shutdown's
   * broadcast machinery.
   *
   * It's a poll rather than a condition variable wait; the field comment of
   * pending_resolve_count explains why that's correct and can't lose a
   * wakeup. */
  while (atomic_load(&raw->pending_resolve_count) > 0) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000}; /* 100us */
    nanosleep(&ts, NULL);
  }

  _ccol_event_loop_teardown_raw(raw);

  /* The slot is released last, only once raw is fully torn down and freed,
   * because the slot's generation bump and the push-back of the free index,
   * and no earlier step, are what make the handle reusable.
   *
   * The slot is fetched again by idx, since a concurrent
   * ccol_event_loop_create_mp may have run its own
   * _ccol_event_loop_handle_slot_acquire in between and reallocated the
   * slots' backing array with cvector_push_back. Any pointer into that array
   * from before this second lock acquisition would then be invalid, while
   * idx itself stays stable. */
  _ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  ccol_event_loop_slot_t *slot2 = (ccol_event_loop_slot_t *)cvector_at(
      ccol_event_loop_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* This raises the generation of this slot past the
      value that the handle of the just-freed loop carried, so that stale
      handle can never again match the generation of a FUTURE acquire for this
      same index. */
  _ccol_event_loop_free_index_release_locked(idx);
  /* Last statement under this lock. The slot's ptr is clear by now, so this
     call does the release the process-exit destructor deferred, and nothing
     below reads the table, so there's no window where the lock is taken again
     and another destroy gets caught. */
  _release_event_loop_slot_table_if_deferred_locked();
  _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
size_t _ccol_event_loop_dispatch_pool_pending_count_for_tests(
    ccol_event_loop loop) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw || !raw->dispatch_pool) {
    if (raw) _ccol_event_loop_resolve_unpin(raw);
    return 0;
  }
  size_t n = ctpool_pending_count(raw->dispatch_pool);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}

/* Returns the thread ID of loop's poller thread, joined or not. After a
 * shutdown a test starts threads until one carries this ID, to prove that
 * the self-call guards don't recognise the poller by its ID. */
_ccol_thread_id_t _ccol_event_loop_poller_thread_for_tests(
    ccol_event_loop loop) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  _ccol_thread_id_t id;
  memset(&id, 0, sizeof(id));
  if (!raw) return id;
  id = raw->poller_thread;
  _ccol_event_loop_resolve_unpin(raw);
  return id;
}

/* Tells whether the calling thread is loop's poller or is running one of
 * loop's dispatch jobs, the same answer the self-call guards of shutdown and
 * destroy get. Returns false for an invalid or stale loop handle. */
bool _ccol_event_loop_test_runs_on_loop_thread(ccol_event_loop loop) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return false;
  bool marked = _ccol_event_loop_job_marked(raw);
  _ccol_event_loop_resolve_unpin(raw);
  return marked;
}

/* Reads how many epoll_wait calls poller_thread has completed so far (see
 * the poller_iterations_for_tests field comment of struct _ccol_event_loop_s).
 * A test samples it twice across a short, bounded window and catches a busy
 * spin directly, because the counter races far ahead, without any flaky
 * wall-clock or CPU measurement. Returns 0 for an invalid or stale loop
 * handle. */
uint64_t _ccol_event_loop_poller_iterations_for_tests(ccol_event_loop loop) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return 0;
  uint64_t n = atomic_load(&raw->poller_iterations_for_tests);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}

/* Reads how many event entries loop's epoch reclaim has freed so far, or 0
 * for an invalid or stale loop handle. */
uint64_t _ccol_event_loop_entries_reclaimed_for_tests(ccol_event_loop loop) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return 0;
  uint64_t n = atomic_load_explicit(&raw->entries_reclaimed_for_tests,
                                    memory_order_relaxed);
  _ccol_event_loop_resolve_unpin(raw);
  return n;
}

/* Stores value into loop's poller_batch_gen. A test uses it to put the
 * counter just below a 32-bit boundary while the poller is idle and then
 * checks that a deferred free past that boundary still gets reclaimed. */
void _ccol_event_loop_set_poller_batch_gen_for_tests(ccol_event_loop loop,
                                                     uint64_t value) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(loop);
  if (!raw) return;
  atomic_store(&raw->poller_batch_gen, value);
  _ccol_event_loop_resolve_unpin(raw);
}

/* Resolves h to the struct _ccol_event_loop_s* behind it WITHOUT a pin: it
 * never touches pending_resolve_count and is just a slot table lookup. That's
 * safe for a test, since test code calling it runs synchronously on one
 * thread with no concurrent destroy to race. With _ccol_event_loop_resolve a
 * test would have to remember a matching _unpin call, which is easy to
 * forget, and a forgotten unpin leaves that loop's pending_resolve_count stuck
 * above zero, so every later ccol_event_loop_destroy against it hangs
 * silently. It returns NULL under exactly the same conditions as
 * _ccol_event_loop_resolve.
 */
struct _ccol_event_loop_s *_ccol_event_loop_resolve_for_tests(
    ccol_event_loop h) {
  _ccol_call_once(ccol_event_loop_slot_table.once,
                  _ccol_event_loop_slot_table_init_globals);
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  _ccol_rw_lock_rdlock(ccol_event_loop_slot_table.rwlock);
  struct _ccol_event_loop_s *raw = NULL;
  if (idx < _event_loop_slot_count_locked()) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  return raw;
}

/* Reads how many slots the ccol_event_loop handle table holds right now,
 * counting slots the table grew as well as freed ones not reused yet, so a
 * test can assert that a churn of creates and destroys reuses freed slots
 * instead of growing the table without bound. */
size_t _ccol_event_loop_slot_table_capacity_for_tests(void) {
  _ccol_call_once(ccol_event_loop_slot_table.once,
                  _ccol_event_loop_slot_table_init_globals);
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  _ccol_rw_lock_rdlock(ccol_event_loop_slot_table.rwlock);
  size_t n = _event_loop_slot_count_locked();
  _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
  return n;
}

/* Test-only hook that makes a pin the test can hold for as long as it wants.
 * It resolves h through the real _ccol_event_loop_resolve, so the pin is
 * real, sets *pinned, waits until the test sets *release, and then unpins.
 *
 * Every real public entry point is quick and bounded, so this module has no
 * naturally slow call that could prove a concurrent destroy really blocks on
 * pending_resolve_count, and a fixed-length pin doesn't prove it either,
 * since a slow machine may spend most of that time before the destroy even
 * starts. With this hook the destroy can't return while *release is clear,
 * however fast or slow the machine. It returns false when h doesn't resolve
 * at all, since there's nothing to pin then. */
bool _ccol_event_loop_resolve_pin_until_for_tests(ccol_event_loop h,
                                                  atomic_int *pinned,
                                                  atomic_int *release) {
  struct _ccol_event_loop_s *raw = _ccol_event_loop_resolve(h);
  if (!raw) return false;
  atomic_store(pinned, 1);
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  while (!atomic_load(release)) nanosleep(&ts, NULL);
  _ccol_event_loop_resolve_unpin(raw);
  return true;
}

/* Test-only. Lock and unlock the write side of ccol_event_loop_slot_table's
 * rwlock directly, bypassing every public API function. They mirror two
 * other pairs: this file's _ccol_event_loop_test_wrlock_reg_slot_for_tests
 * and _wrunlock, which cover one loop's reg_slot_rwlock, and cthreadpool.c's
 * identical ctpool_test_wrlock_slot_table_for_tests and _wrunlock, which
 * cover that module's process-wide slot table.
 *
 * A test can hold this write side from a thread OTHER than the one that will
 * call fork(), for exactly as long as it wants. That's the scenario that
 * deterministically exercises the TID-tracked write-lock reinit of
 * ccol_event_loop_slot_table.rwlock in the in_child branch of
 * _cthreadcomm_atfork_release_impl.
 *
 * There's deliberately no "resolve" step, because there's no per-instance
 * handle to resolve, just the process-wide slot table itself, so the lock
 * call returns void and the unlock call takes no argument. That matches
 * cthreadpool's analogous pair and differs from this file's reg_slot pair.
 * The reg_slot pair's AB-BA hazard doesn't apply here, since neither call
 * touches anything else a concurrent fork() could contend on. */
void _ccol_event_loop_test_wrlock_slot_table_for_tests(void) {
  _ccol_call_once(ccol_event_loop_slot_table.once,
                  _ccol_event_loop_slot_table_init_globals);
  /* Every library path that takes this lock has registered the fork handlers
   * first, so a test taking it through this hook gets the same state
   * whatever tests ran before it. */
  _ccol_call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  _ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
}

void _ccol_event_loop_test_wrunlock_slot_table_for_tests(void) {
  _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
}
#endif

/* The slot table's bookkeeping arrays are freed at process exit; otherwise
 * make memtest's --show-leak-kinds=all reports them as still reachable.
 * While a loop is live the release is deferred to the destroy that frees the
 * last slot (see _cleanup_event_loop_slot_table). Once the table is
 * released, a resolve finds nothing and a create fails cleanly.
 *
 * _ccol_call_once is REQUIRED here, because an __attribute__((destructor))
 * function runs unconditionally for the whole shared object, whichever parts
 * of it the process actually used. Without the guard, a process that links
 * this library but never creates a single ccol_event_loop would lock a mutex
 * here that nothing ever initialised. */
/* Whether any slot still names a loop. The caller holds the write lock.
 *
 * It looks at slot->ptr, not slot->in_use. A destroy clears in_use once its
 * shutdown has drained, so new resolves are rejected, but the rest of the
 * teardown runs after that and ptr is only cleared in its final locked step.
 * A scan that trusted in_use would free this table from under a destroy that
 * is in that window. */
static bool _event_loop_any_slot_live_locked(void) {
  size_t slot_count = _event_loop_slot_count_locked();
  for (size_t i = 0; i < slot_count; i++) {
    ccol_event_loop_slot_t *slot = (ccol_event_loop_slot_t *)cvector_at(
        ccol_event_loop_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* cvector_destroy rather than __cvector_destroy, because it sets the handle
   to NULL as it frees, so a later call sees "already released" here instead
   of indexing a freed vector. The rwlock is deliberately not destroyed,
   since this can run from an ordinary destroy that holds that lock. */
static void _release_event_loop_slot_table_locked(void) {
  cvector_destroy(ccol_event_loop_slot_table.slots);
  cvector_destroy(ccol_event_loop_slot_table.free_indices);
  ccol_event_loop_slot_table.release_deferred = false;
  /* No loop is left and none can be created from here on, so nothing sets
   * or reads the dispatch's two thread-local keys again. A queue destroy sees
   * live false and doesn't read the marker key, a thread that outlives a
   * dlclose() never meets one of this module's keys, and the next load of the
   * library creates fresh ones. */
  if (atomic_exchange(&ccol_event_loop_job_key_bundle.live, false))
    _ccol_thread_ls_key_delete(ccol_event_loop_job_key_bundle.key);
  if (atomic_exchange(&queue_dispatch_marker_bundle.live, false))
    _ccol_thread_ls_key_delete(queue_dispatch_marker_bundle.key);
}

/* Kept behind one out-of-line call so the destroy path that has to make the
   call keeps the code shape it would have without any of this. */
static __attribute__((noinline)) void
_release_event_loop_slot_table_if_deferred_locked(void) {
  if (ccol_event_loop_slot_table.release_deferred &&
      !_event_loop_any_slot_live_locked()) {
    _release_event_loop_slot_table_locked();
  }
}

__attribute__((destructor)) static void _cleanup_event_loop_slot_table(void) {
  _ccol_call_once(ccol_event_loop_slot_table.once,
                  _ccol_event_loop_slot_table_init_globals);
  /* Nothing left to free and nothing safe to touch. The vectors are set to
     NULL as they're destroyed, so a second run stops here. */
  if (!ccol_event_loop_slot_table.slots) return;
  _ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock);
  /* The release only runs once nothing can resolve a handle. The order of
     destructors across translation units isn't ours to decide, and a later
     destructor may hold a live loop that would otherwise find this table
     freed under it. So instead of skipping the release we hand it to
     whichever destroy frees the last slot, and a program that does destroy
     its loops leaves nothing behind whatever order the destructors ran in. */
  if (_event_loop_any_slot_live_locked()) {
    ccol_event_loop_slot_table.release_deferred = true;
    _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
    return;
  }
  _release_event_loop_slot_table_locked();
  _ccol_rw_lock_unlock(ccol_event_loop_slot_table.rwlock);
}
