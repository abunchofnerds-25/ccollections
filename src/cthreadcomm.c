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
 * carries an already-overflowed tv_nsec, INCLUDING a negative one: a
 * caller-supplied timeout with tv_nsec < 0 (a malformed, non-normalised
 * struct timespec, which POSIX never produces itself but nothing here
 * previously rejected) would otherwise flow straight through into the
 * absolute deadline handed to cond_var_timedwait, itself then also
 * possibly negative (undefined behaviour per POSIX for a struct timespec
 * outside [0, 999999999]). Normalised by borrowing whole seconds until
 * tv_nsec is non-negative, the same technique used above for the
 * >= max_nsecs case, just in the other direction. */
void add_duration_to_timespec(struct timespec *target,
                              struct timespec *duration) {
  static const long int max_nsecs = 1000000000;

  if (target->tv_nsec >= max_nsecs) {
    target->tv_sec += target->tv_nsec / max_nsecs;
    target->tv_nsec = target->tv_nsec % max_nsecs;
  } else if (target->tv_nsec < 0) {
    long int borrow = (-target->tv_nsec + max_nsecs - 1) / max_nsecs;
    target->tv_sec -= borrow;
    target->tv_nsec += borrow * max_nsecs;
  }

  /* local copy; avoid mutating caller's struct */
  struct timespec dur = *duration;
  if (dur.tv_nsec >= max_nsecs) {
    dur.tv_sec += dur.tv_nsec / max_nsecs;
    dur.tv_nsec = dur.tv_nsec % max_nsecs;
  } else if (dur.tv_nsec < 0) {
    long int borrow = (-dur.tv_nsec + max_nsecs - 1) / max_nsecs;
    dur.tv_sec -= borrow;
    dur.tv_nsec += borrow * max_nsecs;
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

#ifdef RUNNING_UNIT_TESTS
/* Test-only: widens the window between a queue's own mutex already being
 * held (by whichever of _sendto_cq/_recvfrom_cq/circq_disable_sending/
 * circq_enable_sending called into this function) and this function's own
 * mutex_lock(*w->sel_mtx) a few lines below, from a real handful-of-
 * instructions gap to an arbitrarily long, precisely controlled one. Lets a
 * test deterministically land a concurrent fork() call's own atfork
 * prepare() handler inside this exact window, reproducing the queue-mutex/
 * wait_mtx lock-ordering hazard _cthreadcomm_atfork_prepare's own doc
 * comment describes on demand, rather than relying on timing luck against
 * a window that, in production, is normally far too narrow to hit by
 * chance. See fork_safety.fork_does_not_deadlock_with_queue_registered_
 * before_event_loop in tests/cthreadcomm/tests.c. */
static _Atomic int g_notify_waiter_test_delay_us = 0;
void _notify_waiter_test_set_delay_us(int us) {
  atomic_store(&g_notify_waiter_test_delay_us, us);
}
#endif

/* write(2)/read(2) on this file's own eventfds only ever need to retry an
 * EINTR: eventfd(2) itself guarantees an 8-byte transfer is atomic
 * (all-or-nothing), so unlike a general-purpose fd there is no short-write/
 * short-read case to loop on. A bare (void) cast of the return value used to
 * be enough to document "this notify/drain is deliberately best-effort"
 * under plain glibc, but _FORTIFY_SOURCE's fortified read(2)/write(2)
 * wrappers mark themselves warn_unused_result in a way a (void) cast does
 * not reliably suppress, so the return value must be genuinely consumed;
 * these two helpers do that by feeding it into the EINTR retry condition
 * itself. Every other failure is still deliberately left unhandled, exactly
 * matching every call site's own pre-existing best-effort semantics:
 * EAGAIN on a notify write means the counter is already at its near-
 * UINT64_MAX ceiling, unreachable in practice for this module's own small,
 * bounded increment counts; EAGAIN on a drain read is the expected
 * "nothing pending right now" signal, not an error to retry; EBADF would
 * mean the fd was already closed, which can only indicate a bug elsewhere
 * in this module. */
static void _eventfd_notify(int efd) {
  uint64_t one = 1;
  ssize_t rv;
  do {
    rv = write(efd, &one, sizeof(one));
  } while (rv < 0 && errno == EINTR);
}

static void _eventfd_drain(int efd) {
  uint64_t val;
  ssize_t rv;
  do {
    rv = read(efd, &val, sizeof(val));
  } while (rv < 0 && errno == EINTR);
}

/* Wakes a single waiter node.  Must be called while the owning queue's mutex
 * is held so that the node pointer remains valid throughout. */
static void _notify_waiter(ccol_sel_waiter *w) {
#ifdef RUNNING_UNIT_TESTS
  int delay_us = atomic_load(&g_notify_waiter_test_delay_us);
  if (delay_us > 0) {
    struct timespec ts = {.tv_sec = delay_us / 1000000,
                          .tv_nsec = (long)(delay_us % 1000000) * 1000L};
    nanosleep(&ts, NULL);
  }
#endif
  mutex_lock(*w->sel_mtx);
  *w->ready = true;
  mutex_unlock(*w->sel_mtx);
  cond_var_signal(*w->sel_cond);
  if (w->efd >= 0) {
    _eventfd_notify(w->efd);
  }
}

/* Wakes ONE waiter on a queue's read-or-write waiter list and rotates *rotor
 * so a DIFFERENT waiter is targeted by the next call, cycling through every
 * currently-linked waiter over time rather than always picking the same one.
 * Used for message/slot events where exactly one resource became available;
 * waking more than one waiter would cause a thundering herd.  Must be called
 * while the queue's own mutex is held.
 *
 * *rotor is either NULL (meaning "start a fresh cycle at *head") or a
 * pointer to a node still linked in *this exact list*, an invariant
 * maintained by every unlink path (_sel_unlink_waiter_locked resets *rotor
 * to the removed node's own .next, or to NULL if it had none, whenever the
 * node being removed IS *rotor); never read directly by anything other
 * than this function and that unlink fixup.
 *
 * Fixes a real, silent, permanent starvation bug: a plain "always notify
 * *head" scheme (this function's own pre-rotor behaviour) combined with
 * event_loop's PERMANENTLY-linked registrations (unlike a ccol_select()
 * caller's own transient node, which re-links, and therefore can land in a
 * different list position, every time it must wait again) meant that
 * whichever registration happened to be linked most recently (the most
 * recently added node, since _sel_link_waiter always prepends) stayed *head*
 * (and therefore the ONLY one ever directly notified) for its entire
 * registered lifetime. The pre-existing "cascade" mechanism
 * (_event_loop_queue_cascade_notify_next) only forwards a wake to the next
 * waiter in line when the queue is STILL ready after the just-notified
 * waiter's own callback returns; a callback that keeps up with traffic (even
 * a plain, non-looping single circq_try_recv_zc() per call, an explicitly
 * documented, ordinary pattern) routinely leaves nothing to forward, so
 * every OTHER live registration on the same queue+direction could go
 * uncalled for as long as that one registration remained registered;
 * confirmed via a standalone reproduction: two event_loop_add() calls on one
 * circular_queue's read direction, both callbacks doing a single, immediate
 * circq_try_recv_zc() with no draining loop, fed one message at a time with
 * no backlog ever allowed to build; the second (older) registration received
 * ZERO callbacks across 2000 sent messages, directly contradicting this
 * module's own documented "no live listener is ever passed over
 * indefinitely" guarantee (event_loop_add's own doc comment). Rotating which
 * waiter is targeted on every single notify (independent of, and in
 * addition to, the existing readiness-contingent cascade, which still serves
 * its own distinct purpose of surfacing a backlog a single notify's target
 * might leave stranded) closes this: every live waiter on the list is
 * revisited once per full rotation, so none can be skipped forever purely by
 * virtue of a sibling registration's own callback keeping the queue
 * drained. */
static void notify_one_sel_waiter(ccol_sel_waiter **head,
                                  ccol_sel_waiter **rotor) {
  ccol_sel_waiter *target = *rotor ? *rotor : *head;
  if (!target) return;
  _notify_waiter(target);
  /* Wraps to NULL once the rotation walks off the tail (target->next ==
   * NULL), so the very next call starts a fresh cycle at *head again;
   * *head itself may have changed since this cycle began (new waiters
   * prepend there), which is fine: a full cycle only needs to eventually
   * reach every CURRENTLY-linked waiter, not preserve a fixed traversal
   * order across cycles. */
  *rotor = target->next;
}

/* Wakes every thread currently blocked in ccol_select on this queue.  Used
 * exclusively for state-change events (disable_sending, enable_sending) where
 * every blocked thread must re-evaluate regardless of resource availability.
 * Must be called while the queue's own mutex is held. */
static void notify_all_sel_waiters(ccol_sel_waiter *head) {
  for (ccol_sel_waiter *w = head; w != NULL; w = w->next) _notify_waiter(w);
}

/* ========================================================================== */
/*         QUEUE MUTEX FORK SAFETY (pthread_atfork) registry                  */
/* ========================================================================== */

/* Process-wide registry of every live circular_queue's/dynamic_queue's own
 * mutex address (mutex_t*), so a lazily-registered at_fork() handler can
 * lock every one of them before fork() proceeds and unlock them again
 * immediately afterward, in both the parent and the child.
 *
 * fork() duplicates only the calling thread; any lock some OTHER thread
 * held at that instant (here, a cq->mutex/dq->mutex held mid-send/recv by
 * a thread that isn't the one calling fork()) is inherited by the child
 * already locked, with no thread left alive there that could ever unlock
 * it. Every future operation on that same queue in the child
 * (circq_send_zc/circq_recv_zc/dynmq_send_zc/..., all of which start with
 * mutex_lock(cq->mutex)/mutex_lock(dq->mutex)) then hangs forever. This is
 * the identical hazard event_loop's own atfork handling already closes for
 * every lock IT owns (including, for a queue-backed registration, that
 * reg's own wait_mtx); this registry closes the same gap for the queue's
 * own mutex, which wait_mtx was never a substitute for: wait_mtx only
 * guards the generic _notify_waiter() ready-flag handshake, while
 * msg_count/writing_disabled/the waiter lists themselves all live behind
 * cq->mutex/dq->mutex, previously unprotected by any atfork handler at
 * all, event_loop-registered or not.
 *
 * channel needs no entry of its own: it is backed by two circular_queue
 * instances, each already registered here via circular_queue_create_with_
 * mprocs, so both directions of a channel are covered automatically.
 *
 * Unlike event_loop_slot_table, this registry has no generation/handle
 * concept to maintain (circular_queue/dynamic_queue are still plain
 * pointers, not opaque handles); it is nothing more than an unordered bag
 * of live mutex_t* addresses, added at construction and removed at
 * destruction, purely so the merged atfork handler (see
 * _cthreadcomm_atfork_prepare, defined further below once struct
 * event_loop_s is fully declared) has something to walk. No is_child-only
 * fixup is needed the way event_loop's own child-release path needs one
 * (marking foreign_since_fork, replacing its epfd, ...): a circular_queue/
 * dynamic_queue owns no kernel object and no thread of its own, so once its
 * mutex is correctly unlocked post-fork, its entire state (msg_count, the
 * message array, the waiter lists) is simply whatever it was at the fork
 * instant, exactly as safe to keep using in the child as in the parent.
 * The same reasoning already applies, unremarked, to every plain condition
 * variable this module owns (e.g. read_cond/write_cond here, or
 * event_loop's own joined_cv): only a LOCK actually held by a now-vanished
 * thread is a hazard; a condvar is never "locked" in the first place, so
 * nothing needs to be done to one across fork() at all.
 *
 * IMPORTANT: this registry's own atfork involvement is NOT registered via
 * its own independent at_fork() call. An earlier version of this fix did
 * exactly that (a second, wholly separate at_fork() triple alongside
 * event_loop's own pre-existing one) and was a real, reproduced deadlock:
 * pthread_atfork's prepare handlers run in REVERSE registration order, so
 * which of the two independent handler sets runs first is purely an
 * accident of which subsystem (a circular_queue, or an event_loop) happens
 * to be used first in a given process. When event_loop's own prepare runs
 * first, it locks a queue-backed registration's own reg->wait_mtx; when
 * this registry's own (then-separate) prepare ran second, it locked that
 * SAME queue's cq->mutex. That is the reverse of the order every ordinary
 * code path uses: _notify_waiter (called by circq_send_zc/recv_zc/etc.
 * from an entirely unrelated, concurrently-running thread) always locks
 * cq->mutex first, then reg->wait_mtx. Confirmed via a standalone
 * reproduction (a queue created before the first event_loop in the
 * process, then registered with one, with a real circq_send_zc racing a
 * fork() call): the forking thread deadlocked inside fork() itself
 * (gdb showed it blocked locking cq->mutex from what was then this
 * registry's own separate prepare handler, while the sending thread was
 * simultaneously blocked locking reg->wait_mtx from inside _notify_waiter,
 * a textbook AB-BA cycle). Fixed by merging both subsystems' locking into
 * ONE _cthreadcomm_atfork_prepare/_release/_child_release triple (defined
 * further below), registered via a single at_fork() call, which locks
 * every queue's own mutex in the one position consistent with EVERY real
 * nested-locking pattern in this file: after the stripe lock that owns any
 * of its event_loop registrations (matching _event_loop_add_queue's/
 * _event_loop_remove_unlink's own stripe->lock-then-cq->mutex nesting) and
 * before any of those registrations' own wait_mtx (matching
 * _notify_waiter's own cq->mutex-then-wait_mtx nesting); see that
 * function's own comment for the full three-phase design. This registry's
 * own struct and add/remove functions stay here, close to circular_queue/
 * dynamic_queue's own code; only the actual locking-across-fork logic
 * moved to live alongside event_loop's, since it now depends on struct
 * event_loop_s's full definition. */
#if FORK_SAFETY_REQUIRED
static struct {
  mutex_t mutex;
  once_flag_t once;
  cvec addrs; /* cvec of mutex_t*; unordered, swap-removed on delete */
} queue_mutex_registry = {0};
#endif

/* Guards the ONE, shared at_fork() registration covering both this
 * registry's own queue mutexes and event_loop_slot_table's own locks (see
 * queue_mutex_registry's own doc comment for why this must be a single,
 * merged registration rather than two independent ones). Defined here
 * (rather than only where event_loop_slot_table lives) so that
 * _queue_mutex_registry_add/_remove (reachable from a process that never
 * touches event_loop at all) can trigger it on their own, without
 * requiring event_loop_slot_table to already exist first. */
static once_flag_t g_cthreadcomm_atfork_once = ONCE_INIT;

/* Forward declaration: registers the merged at_fork() triple; body defined
 * further below, once struct event_loop_s is fully declared (the merged
 * prepare/release functions it wires up dereference loop->shutdown_lock/
 * reg_slot_mutex/stripes[]). Internally also lazily initialises
 * event_loop_slot_table's own plain data (mutex + both cvecs) via its own
 * call_once, exactly mirroring how this function's own caller already
 * lazily initialises queue_mutex_registry's, so whichever subsystem is
 * used first in a process still ends up with BOTH structures fully ready
 * before the shared at_fork() handlers can ever run. */
static void _cthreadcomm_register_atfork_once(void);

#if FORK_SAFETY_REQUIRED
static void _queue_mutex_registry_init_globals(void) {
  mutex_init(queue_mutex_registry.mutex);
  queue_mutex_registry.addrs = cvector_create(sizeof(mutex_t *), NULL);
  if (!queue_mutex_registry.addrs)
    fatal_err("queue mutex registry: failed to allocate address vector");
}

/* Registers m so a future fork() locks it in _cthreadcomm_atfork_prepare.
 * Called once per live queue, right after its mutex is initialised and
 * every other field of the owning queue is otherwise already fully
 * constructed, so a failure here can be unwound exactly like any other
 * allocation failure in that same constructor. Returns false on failure
 * (this registry's own cvector_push_back call failing to grow); the caller
 * must treat that exactly like any other construction-time allocation
 * failure: tear down what it already built and report an ordinary,
 * graceful error, rather than aborting the process over what is, from the
 * queue constructor's own perspective, an entirely ordinary OOM case. */
static bool _queue_mutex_registry_add(mutex_t *m) {
  call_once(queue_mutex_registry.once, _queue_mutex_registry_init_globals);
  call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  mutex_lock(queue_mutex_registry.mutex);
  bool ok = cvector_push_back(queue_mutex_registry.addrs, &m) == ccol_success;
  mutex_unlock(queue_mutex_registry.mutex);
  return ok;
}

/* Unregisters m (previously added by a _queue_mutex_registry_add(m) call).
 * Swap-removes with the registry's current last entry rather than shifting
 * every following one down: this registry is an unordered bag with no
 * iteration-order contract for _cthreadcomm_atfork_prepare/_release to
 * preserve. A no-op if m is somehow not present (defensive; every real
 * caller only ever removes an address it itself successfully added exactly
 * once). */
static void _queue_mutex_registry_remove(mutex_t *m) {
  call_once(queue_mutex_registry.once, _queue_mutex_registry_init_globals);
  call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  mutex_lock(queue_mutex_registry.mutex);
  size_t n = cvector_elem_count(queue_mutex_registry.addrs);
  for (size_t i = 0; i < n; i++) {
    mutex_t **slot = (mutex_t **)cvector_at(queue_mutex_registry.addrs, i);
    if (*slot != m) continue;
    mutex_t *last = NULL;
    cvector_pop_back(queue_mutex_registry.addrs, &last);
    if (i != n - 1) {
      mutex_t **refetched =
          (mutex_t **)cvector_at(queue_mutex_registry.addrs, i);
      *refetched = last;
    }
    break;
  }
  mutex_unlock(queue_mutex_registry.mutex);
}

/* Frees the registry's own backing array at process exit, so make memtest's
 * --show-leak-kinds=all does not report it as still-reachable; mirrors
 * _cleanup_event_loop_slot_table exactly (see that function's own comment
 * for the full rationale, including why this is sound only given every
 * queue the application created was itself destroyed before process exit).
 */
__attribute__((destructor)) static void _cleanup_queue_mutex_registry(void) {
  call_once(queue_mutex_registry.once, _queue_mutex_registry_init_globals);
  mutex_lock(queue_mutex_registry.mutex);
  __cvector_destroy(queue_mutex_registry.addrs);
  mutex_unlock(queue_mutex_registry.mutex);
}
#endif /* FORK_SAFETY_REQUIRED */

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

  /* Round-robin cursors used by notify_one_sel_waiter (see its own doc
   * comment); NULL means "start a fresh cycle at the corresponding head". */
  ccol_sel_waiter *sel_read_rotor;
  ccol_sel_waiter *sel_write_rotor;
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

  /* circular_queue_create_with_mprocs backs the queue with a single
   * max_size * sizeof(c_message_t) array, computed with a plain
   * multiplication rather than an overflow-checked allocator (calloc);
   * without this guard, a max_size in roughly [max_elem_count / 16,
   * max_elem_count] (i.e. the top slice of the very range this function
   * has just accepted above) makes that multiplication wrap size_t,
   * handing a tiny or even zero-sized request to the allocator while the
   * queue still believes it has room for max_size messages, corrupting
   * the heap on the very first send. Mirrors the identical
   * SIZE_MAX / element_size guard cvector.c/csort.c/cmempool.c already use
   * for the same class of allocation. */
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
  cq->sel_read_rotor = NULL;
  cq->sel_write_rotor = NULL;

#if FORK_SAFETY_REQUIRED
  /* Registered as the LAST step, after cq is otherwise fully constructed,
   * so a failure here can be unwound exactly like any earlier allocation
   * failure in this same function; see _queue_mutex_registry_add's own
   * comment for why this is a graceful, non-fatal failure mode. */
  if (!_queue_mutex_registry_add(&cq->mutex)) {
    if (err_str) {
      *err_str = CCOL_ERR_STR(
          "Failed to register circular_queue's mutex for fork safety");
    }
    mutex_destroy(cq->mutex);
    cond_var_destroy(cq->read_cond);
    cond_var_destroy(cq->write_cond);
    _mem_free(mmgmt_procs, cq->msg_array);
    _mem_free(mmgmt_procs, cq->m_procs);
    _mem_free(mmgmt_procs, cq);
    return NULL;
  }
#endif

  if (err_str) {
    *err_str = NULL;
  }

  return cq;
}

/* Destroys the circular queue. Asserts if any messages remain unconsumed
 * (their data pointers would be leaked), or if a ccol_select()/event_loop
 * waiter is still linked into either waiter list. The second case is a real
 * use-after-free hazard, not just a leak: a still-linked waiter node's own
 * sel_mtx points at &cq->mutex, so destroying cq out from under it (rather
 * than requiring the caller to event_loop_remove()/let ccol_select() return
 * first) leaves that node holding a dangling pointer that the next touch of
 * it (event_loop_remove, __event_loop_destroy's own teardown walk, or
 * ccol_select_timed's own Phase 3 deregister) would dereference. Both
 * conditions are caller bugs and must be made visible here, before the
 * memory is actually freed, rather than surfacing later as corruption. */
void __circular_queue_destroy(circular_queue *cq) {
  if (cq) {
    if (circq_msg_count(cq) > 0) {
      ccol_assert(false);
    }

    /* Read under the mutex: a concurrent ccol_select() call's own Phase 1/3
     * (or a producer/consumer's notify_one_sel_waiter) could be linking or
     * unlinking a node in either list at this exact moment, so this must be
     * a genuine, race-free read, not an unlocked peek at fields a different
     * thread might be updating. */
    mutex_lock(cq->mutex);
    bool has_sel_waiters = (cq->sel_read_waiters_head != NULL) ||
                           (cq->sel_write_waiters_head != NULL);
    mutex_unlock(cq->mutex);
    if (has_sel_waiters) {
      ccol_assert(false);
    }

    if (cq->msg_array) {
      _mem_free(cq->m_procs, cq->msg_array);
      cq->msg_array = NULL;
    }

#if FORK_SAFETY_REQUIRED
    /* Unregister before destroying the mutex: once destroyed, &cq->mutex
     * must never again be a candidate for _queue_atfork_prepare to lock. */
    _queue_mutex_registry_remove(&cq->mutex);
#endif

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

#ifdef RUNNING_UNIT_TESTS
/* Test-only: lock/unlock cq's own internal mutex directly, bypassing every
 * public API function, so a test can hold it locked for an arbitrarily
 * long, precisely controlled window. See their own doc comments in
 * cthreadcomm.h. */
void circq_test_lock_mutex_for_tests(circular_queue *cq) {
  mutex_lock(cq->mutex);
}

void circq_test_unlock_mutex_for_tests(circular_queue *cq) {
  mutex_unlock(cq->mutex);
}
#endif

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
  notify_one_sel_waiter(&cq->sel_read_waiters_head, &cq->sel_read_rotor);
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

/* Forward declaration: defined below, right before circq_recv_zc; needed
 * here for circq_timed_send_zc's own RUNNING_UNIT_TESTS-only
 * racing-consumer simulation. */
void _recvfrom_cq(circular_queue *cq, c_message_t *target_buf);

#ifdef RUNNING_UNIT_TESTS
/* Test-only hooks: force the very next cond_var_timedwait call inside
 * circq_timed_send_zc's own wait loop to report EINVAL instead of a real
 * wait outcome, then auto-disarm. The _racing_ready variant additionally
 * frees one slot's worth of queue accounting (via _recvfrom_cq, exactly
 * what a real concurrent consumer completing its receive would do) under
 * the same cq->mutex this call already holds, simulating that consumer's
 * own wakeup having legitimately completed an instant before the
 * unrelated, forced error is observed; mirroring
 * ccol_select_test_force_next_condvar_wait_error_racing_ready's own
 * reasoning exactly: the real wait call is skipped entirely rather than
 * actually releasing the mutex, so a genuinely concurrent thread can never
 * race in here on its own; this is the only way to deterministically
 * reach that interleaving from a test. The freed slot's own message data
 * (if any) is handed back out via *racing_ready_msg_out so a test can
 * verify it, since it would otherwise be silently discarded here. */
static _Atomic bool g_circq_send_force_condvar_wait_error = false;
static _Atomic bool g_circq_send_force_condvar_wait_error_also_race = false;
static c_message_t g_circq_send_race_freed_msg = {0};

void circq_test_force_next_send_condvar_wait_error(void) {
  atomic_store(&g_circq_send_force_condvar_wait_error, true);
}

void circq_test_force_next_send_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_circq_send_force_condvar_wait_error, true);
  atomic_store(&g_circq_send_force_condvar_wait_error_also_race, true);
}

c_message_t circq_test_take_race_freed_msg(void) {
  c_message_t m = g_circq_send_race_freed_msg;
  g_circq_send_race_freed_msg = (c_message_t){0};
  return m;
}
#endif

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
#ifdef RUNNING_UNIT_TESTS
      if (atomic_load(&g_circq_send_force_condvar_wait_error)) {
        atomic_store(&g_circq_send_force_condvar_wait_error, false);
        retval = EINVAL;
        if (atomic_load(&g_circq_send_force_condvar_wait_error_also_race)) {
          atomic_store(&g_circq_send_force_condvar_wait_error_also_race, false);
          _recvfrom_cq(cq, &g_circq_send_race_freed_msg);
        }
      } else {
        retval = cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
      }
#else
      retval = cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
#endif
      if (retval) {
        /* Re-check under the mutex before committing to either outcome
         * below, regardless of which one cond_var_timedwait's own return
         * value suggests: cond_var_timedwait always re-acquires cq->mutex
         * before returning, whether it succeeds or fails, so a concurrent
         * consumer's own _recvfrom_cq (which itself requires cq->mutex to
         * free a slot and signal write_cond) can legitimately complete and
         * hand the mutex back to this exact call the instant before an
         * unrelated, spurious non-ETIMEDOUT error is also reported;
         * without this re-check, that genuine, already-available slot
         * would be silently discarded and reported as
         * ccol_unexpected_failure instead of actually being used to send.
         * Mirrors _sel_wait_condvar's own identical FAILURE-branch
         * reasoning. Also covers the ordinary ETIMEDOUT case, where a
         * consumer may have freed a slot (or writing_disabled changed)
         * between the kernel detecting the expiry and us reacquiring the
         * mutex. */
        if (cq->msg_count < cq->max_size || cq->writing_disabled) break;
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
  notify_one_sel_waiter(&cq->sel_write_waiters_head, &cq->sel_write_rotor);
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

#ifdef RUNNING_UNIT_TESTS
/* Test-only hooks: force the very next cond_var_timedwait call inside
 * circq_timed_recv_zc's own wait loop to report EINVAL instead of a real
 * wait outcome, then auto-disarm. The _racing_ready variant additionally
 * enqueues a sentinel message (via _sendto_cq, exactly what a real
 * concurrent producer completing its send would do) under the same
 * cq->mutex this call already holds, simulating that producer's own
 * wakeup having legitimately completed an instant before the unrelated,
 * forced error is observed; mirrors
 * ccol_select_test_force_next_condvar_wait_error_racing_ready's own
 * reasoning (the real wait call is skipped entirely rather than actually
 * releasing the mutex, so a genuinely concurrent thread can never race in
 * here on its own). */
static _Atomic bool g_circq_recv_force_condvar_wait_error = false;
static _Atomic bool g_circq_recv_force_condvar_wait_error_also_race = false;

void circq_test_force_next_recv_condvar_wait_error(void) {
  atomic_store(&g_circq_recv_force_condvar_wait_error, true);
}

void circq_test_force_next_recv_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_circq_recv_force_condvar_wait_error, true);
  atomic_store(&g_circq_recv_force_condvar_wait_error_also_race, true);
}
#endif

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
        retval = cond_var_timedwait(cq->read_cond, cq->mutex, abs_time);
      }
#else
      retval = cond_var_timedwait(cq->read_cond, cq->mutex, abs_time);
#endif
      if (retval) {
        /* Re-check under the mutex before committing to either outcome
         * below, regardless of which one cond_var_timedwait's own return
         * value suggests: cond_var_timedwait always re-acquires cq->mutex
         * before returning, whether it succeeds or fails, so a concurrent
         * producer's own _sendto_cq (which itself requires cq->mutex to
         * add a message and signal read_cond) can legitimately complete
         * and hand the mutex back to this exact call the instant before
         * an unrelated, spurious non-ETIMEDOUT error is also reported;
         * without this re-check, that genuine, already-arrived message
         * would be silently discarded and reported as
         * ccol_unexpected_failure instead of actually being received.
         * Mirrors _sel_wait_condvar's own identical FAILURE-branch
         * reasoning. Also covers the ordinary ETIMEDOUT case, where a
         * producer may have added a message between the kernel detecting
         * the expiry and us reacquiring the mutex. */
        if (cq->msg_count > 0) break;
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

  /* Round-robin cursors used by notify_one_sel_waiter (see its own doc
   * comment); NULL means "start a fresh cycle at the corresponding head". */
  ccol_sel_waiter *sel_read_rotor;
  ccol_sel_waiter *sel_write_rotor;
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
  dq->sel_read_rotor = NULL;
  dq->sel_write_rotor = NULL;

#if FORK_SAFETY_REQUIRED
  /* Registered as the LAST step, after dq is otherwise fully constructed;
   * see circular_queue_create_with_mprocs's identical call for the full
   * rationale (mirrored here exactly). dynamic_queue has no msg_array of
   * its own to unwind (it starts as an empty linked list). */
  if (!_queue_mutex_registry_add(&dq->mutex)) {
    if (err_str) {
      *err_str = CCOL_ERR_STR(
          "Failed to register dynamic_queue's mutex for fork safety");
    }
    mutex_destroy(dq->mutex);
    cond_var_destroy(dq->read_cond);
    _mem_free(mmgmt_procs, dq->m_procs);
    _mem_free(mmgmt_procs, dq);
    return NULL;
  }
#endif

  if (err_str) {
    *err_str = NULL;
  }

  return dq;
}

/* Destroys the dynamic queue. Like __circular_queue_destroy, asserts if any
 * messages remain to make uncleaned-up data pointers visible as a bug, and
 * likewise asserts if a ccol_select()/event_loop waiter is still linked into
 * either waiter list; see __circular_queue_destroy's own comment for why the
 * latter is a real use-after-free hazard (a dangling &dq->mutex in the
 * still-linked node), not merely a leak. */
void __dynamic_queue_destroy(dynamic_queue *dq) {
  if (dq) {
    if (dynmq_msg_count(dq) > 0) {
      ccol_assert(false);
    }

    /* Read under the mutex; see __circular_queue_destroy's identical check
     * for why this must be a genuine, race-free read rather than an
     * unlocked peek. */
    mutex_lock(dq->mutex);
    bool has_sel_waiters = (dq->sel_read_waiters_head != NULL) ||
                           (dq->sel_write_waiters_head != NULL);
    mutex_unlock(dq->mutex);
    if (has_sel_waiters) {
      ccol_assert(false);
    }

#if FORK_SAFETY_REQUIRED
    /* Unregister before destroying the mutex; see
     * __circular_queue_destroy's identical call for why. */
    _queue_mutex_registry_remove(&dq->mutex);
#endif

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
    notify_one_sel_waiter(&dq->sel_read_waiters_head, &dq->sel_read_rotor);
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
    notify_one_sel_waiter(&dq->sel_write_waiters_head, &dq->sel_write_rotor);
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

#ifdef RUNNING_UNIT_TESTS
/* Test-only hooks: force the very next cond_var_timedwait call inside
 * dynmq_timed_recv_zc's own wait loop to report EINVAL instead of a real
 * wait outcome, then auto-disarm. The _racing_ready variant additionally
 * enqueues a sentinel message (via _sendto_dq, exactly what a real
 * concurrent producer completing its send would do) under the same
 * dq->mutex this call already holds, simulating that producer's own
 * wakeup having legitimately completed an instant before the unrelated,
 * forced error is observed; mirrors
 * ccol_select_test_force_next_condvar_wait_error_racing_ready's own
 * reasoning (the real wait call is skipped entirely rather than actually
 * releasing the mutex, so a genuinely concurrent thread can never race in
 * here on its own). */
static _Atomic bool g_dynmq_recv_force_condvar_wait_error = false;
static _Atomic bool g_dynmq_recv_force_condvar_wait_error_also_race = false;

void dynmq_test_force_next_recv_condvar_wait_error(void) {
  atomic_store(&g_dynmq_recv_force_condvar_wait_error, true);
}

void dynmq_test_force_next_recv_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_dynmq_recv_force_condvar_wait_error, true);
  atomic_store(&g_dynmq_recv_force_condvar_wait_error_also_race, true);
}
#endif

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
        retval = cond_var_timedwait(dq->read_cond, dq->mutex, abs_time);
      }
#else
      retval = cond_var_timedwait(dq->read_cond, dq->mutex, abs_time);
#endif
      if (retval) {
        /* Re-check under the mutex before committing to either outcome
         * below, regardless of which one cond_var_timedwait's own return
         * value suggests: cond_var_timedwait always re-acquires dq->mutex
         * before returning, whether it succeeds or fails, so a concurrent
         * producer's own _sendto_dq (which itself requires dq->mutex to
         * add a message and signal read_cond) can legitimately complete
         * and hand the mutex back to this exact call the instant before
         * an unrelated, spurious non-ETIMEDOUT error is also reported;
         * without this re-check, that genuine, already-arrived message
         * would be silently discarded and reported as
         * ccol_unexpected_failure instead of actually being received.
         * Mirrors _sel_wait_condvar's own identical FAILURE-branch
         * reasoning. Also covers the ordinary ETIMEDOUT case, where a
         * producer may have added a message between the kernel detecting
         * the expiry and us reacquiring the mutex. */
        if (dq->msg_count > 0) break;
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

/* Splices node out of a waiter doubly-linked list. Caller must already hold
 * the owning queue's mutex; see _sel_unlink_waiter below for the ordinary,
 * self-locking wrapper used wherever the splice doesn't need to be combined
 * with another mutation under the same critical section.
 *
 * rotor must be the address of the SAME list's own round-robin cursor (see
 * notify_one_sel_waiter's doc comment); if node is the current rotor target,
 * it is advanced to node's own .next (or NULL, restarting the cycle at
 * *head) BEFORE node is spliced out. This is required, not optional: once
 * node is unlinked, it may be freed (an event_loop registration) or simply
 * go out of scope (a ccol_select() waiter's heap-allocated node array is
 * freed once that call returns) the moment this function returns, so a
 * rotor left dangling on it would be a use-after-free/stack-invalid read the
 * very next time notify_one_sel_waiter dereferences it. */
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

/* Splices node out of a waiter doubly-linked list under the queue's mutex.
 * head/rotor must be the addresses of the appropriate sel_{read,write}_
 * waiters_head/sel_{read,write}_rotor pair in the owning queue. */
static void _sel_unlink_waiter(ccol_sel_waiter *node, ccol_sel_waiter **head,
                               ccol_sel_waiter **rotor, mutex_t *mtx) {
  mutex_lock(*mtx);
  _sel_unlink_waiter_locked(node, head, rotor);
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
    ccol_sel_waiter **rotor = (selectables[i].dir == ccol_select_read)
                                  ? &cq->sel_read_rotor
                                  : &cq->sel_write_rotor;
    _sel_unlink_waiter(&nodes[i], head, rotor, &cq->mutex);
  } else {
    dynamic_queue *dq = selectables[i].dq;
    ccol_sel_waiter **head = (selectables[i].dir == ccol_select_read)
                                 ? &dq->sel_read_waiters_head
                                 : &dq->sel_write_waiters_head;
    ccol_sel_waiter **rotor = (selectables[i].dir == ccol_select_read)
                                  ? &dq->sel_read_rotor
                                  : &dq->sel_write_rotor;
    _sel_unlink_waiter(&nodes[i], head, rotor, &dq->mutex);
  }

  /* Drain without closing: the eventfd is reused across iterations.
   * EFD_NONBLOCK is set, so this read returns EAGAIN when the counter is
   * already 0 (the producer wrote while we were processing another event).
   * The drain and the producer's write are both serialised by the queue
   * mutex, so there is no race between them. */
  if (nodes[i].efd >= 0) {
    _eventfd_drain(nodes[i].efd);
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
  /* ccol_select_timed() backs its own per-selectable waiter-node array with
   * a single, plain (non-overflow-checked) n * sizeof(ccol_sel_waiter)
   * malloc() a few lines below its own call into this function; guard the
   * multiplication itself here, before any allocation is attempted, mirroring
   * the identical SIZE_MAX / element_size idiom
   * verify_circular_queue_create_inputs/event_loop_create_with_mprocs
   * already use for the same class of allocation elsewhere in this file.
   * Also covers _sel_setup_epoll's own n_fd * sizeof(_sel_fd_pair)
   * allocation transitively, since n_fd <= n and sizeof(_sel_fd_pair) <=
   * sizeof(ccol_sel_waiter); that function carries its own independent
   * guard too rather than relying solely on this one. */
  if (n > SIZE_MAX / sizeof(ccol_sel_waiter)) return ccol_invalid_args;
  /* _sel_phase1_scan_register narrows a matched selectable's own index (a
   * size_t, 0..n-1) into a plain `int` (its `found` local, doubling as the
   * -1/-2 "nothing found yet"/"system error" sentinels), which
   * ccol_select_timed then widens back via `(size_t)found`; an n large
   * enough to produce a match index beyond INT_MAX would silently narrow to
   * a negative or wrapped value there, corrupting *ready_index. The
   * SIZE_MAX / sizeof(ccol_sel_waiter) guard just above does not cover this:
   * that bound is many orders of magnitude larger than INT_MAX on any
   * 64-bit platform. Mirrors event_loop_create_with_mprocs's own identical
   * n > INT_MAX rejection for max_events_per_wait, guarding the same class
   * of size_t-into-int narrowing. */
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

/* Base epoll interest flags for one direction on an fd selectable; shared
 * between _sel_setup_epoll (registration) and _sel_wait_epoll (resolving a
 * fired combined-fd event back to the one member selectable it satisfies),
 * so the two can never drift out of sync with each other. */
static uint32_t _sel_fd_base_events(ccol_select_dir dir) {
  return (dir == ccol_select_read)
             ? (uint32_t)(EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)
             : (uint32_t)(EPOLLOUT | EPOLLERR | EPOLLHUP);
}

/* Tag bit stamped into an fd-group epoll registration's ev.data.u64, with
 * the real fd value packed into the remaining 63 bits. Never collides with
 * a queue selectable's ev.data.u64 (a plain index into selectables[],
 * always < n; see _sel_ensure_efd): n can never realistically approach
 * 2^63, since the caller would first need a real selectables[] array of
 * that many elements. This is what lets _sel_wait_epoll tell "an fd fired"
 * apart from "a queue eventfd fired" from the bare ev.data.u64 value alone,
 * with no separate side table to consult. */
#define _SEL_FD_TAG ((uint64_t)1 << 63)

/* One fd selectable, paired with its own index into selectables[] so that,
 * after sorting an array of these by fd, every selectable sharing one real
 * fd (any mix of directions, including outright duplicates) sorts into one
 * contiguous run; see _sel_setup_epoll. */
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

/* Creates an epoll instance and registers every fd selectable with
 * level-triggered interest flags. Selectables that share the same
 * underlying fd (e.g. a caller watching one connected socket for both
 * readability and writability in a single call, or two selectables
 * outright duplicating each other) are combined into a SINGLE epoll_ctl
 * registration carrying the OR of every member's own interest mask,
 * mirroring how event_loop_add already combines a read and a write
 * registration sharing one fd into one entry. Without this combining, a
 * second EPOLL_CTL_ADD for an fd already registered earlier in the same
 * call fails outright with EEXIST, which is not a caller error (nothing
 * about the public API forbids or even documents this as unsupported) and
 * is a completely ordinary, common request (wait for either direction on
 * one fd, whichever comes first).
 *
 * Selectables are grouped by fd via a sort (qsort, O(n log n)) rather than
 * an O(n)-per-lookup linear scan for "is this fd already registered",
 * specifically so the common case (many distinct fds, no sharing at all)
 * does not degrade to O(n^2); see this project's own standing policy that
 * reducible algorithmic complexity is treated as a bug even when today's
 * typical n is small.
 *
 * Returns the epfd on success or -1 on any system error. Closing the
 * returned epfd auto-removes all registered fds. */
static int _sel_setup_epoll(size_t n, ccol_selectable *selectables) {
  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) return -1;

  size_t n_fd = 0;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == ccol_selectable_fd) n_fd++;
  }
  if (n_fd == 0) return epfd;

  /* Guards the n_fd * sizeof(_sel_fd_pair) multiplication below against
   * size_t overflow. Already unreachable given _sel_validate_args's own
   * n * sizeof(ccol_sel_waiter) guard (n_fd <= n and sizeof(_sel_fd_pair) <=
   * sizeof(ccol_sel_waiter)), kept here anyway as an explicit, local guard
   * at the actual point of allocation rather than relying solely on a
   * transitive proof from a distant caller; mirrors this file's own
   * SIZE_MAX / element_size idiom used elsewhere. */
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

    struct epoll_event ev;
    ev.data.u64 = _SEL_FD_TAG | (uint64_t)(unsigned int)fd;
    ev.events = combined;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
      free(pairs);
      close(epfd);
      return -1;
    }
    i = j;
  }

  free(pairs);
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
          notify_one_sel_waiter(&cq->sel_read_waiters_head,
                                &cq->sel_read_rotor);
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
          notify_one_sel_waiter(&cq->sel_write_waiters_head,
                                &cq->sel_write_rotor);
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
          notify_one_sel_waiter(&dq->sel_read_waiters_head,
                                &dq->sel_read_rotor);
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
          notify_one_sel_waiter(&dq->sel_write_waiters_head,
                                &dq->sel_write_rotor);
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

typedef enum {
  _SEL_CONDVAR_READY,     /* woke normally (or infinite wait); proceed to
                            Phase 3 */
  _SEL_CONDVAR_TIMED_OUT, /* deadline elapsed with *ready still false */
  _SEL_CONDVAR_FAILURE,   /* cond_var_timedwait returned an unexpected
                            (non-zero, non-ETIMEDOUT) error */
} _sel_condvar_outcome;

#ifdef RUNNING_UNIT_TESTS
/* Test-only hook: forces the very next cond_var_timedwait call inside
 * _sel_wait_condvar's deadline branch to report EINVAL instead of actually
 * waiting, then auto-disarms. Exists specifically to make the "an unexpected
 * error, not 0 or ETIMEDOUT, comes back from cond_var_timedwait" path
 * deterministically reachable from a test: no legitimate call reachable from
 * this codebase's own deadline computation (_sel_compute_deadline, which
 * always normalises tv_nsec into [0, 1e9) before it is ever handed to
 * cond_var_timedwait) can trigger a genuine EINVAL here. */
static _Atomic bool g_sel_force_condvar_wait_error = false;

/* Test-only hook, consumed alongside g_sel_force_condvar_wait_error: when
 * both fire together, the injected error also sets *ready = true just before
 * being reported, simulating a producer's notify having legitimately
 * completed (under the same sel_mtx this call already holds) an instant
 * before the unrelated, forced error is observed. This is the only way to
 * deterministically reach that interleaving from a test: the real
 * cond_var_timedwait call this hook replaces is skipped entirely rather than
 * actually releasing sel_mtx, so a genuinely concurrent producer thread's own
 * _notify_waiter (which itself requires sel_mtx) can never actually race in
 * here on its own. See _sel_wait_condvar's own FAILURE-branch comment for
 * why *ready must win over a same-instant unexpected error. */
static _Atomic bool g_sel_force_condvar_wait_error_also_marks_ready = false;

void ccol_select_test_force_next_condvar_wait_error(void) {
  atomic_store(&g_sel_force_condvar_wait_error, true);
}

void ccol_select_test_force_next_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_sel_force_condvar_wait_error, true);
  atomic_store(&g_sel_force_condvar_wait_error_also_marks_ready, true);
}
#endif

/* Waits on sel_cond until *ready is set by a producer or the deadline
 * elapses. Returns _SEL_CONDVAR_TIMED_OUT if the deadline elapsed with
 * *ready still false, _SEL_CONDVAR_FAILURE if cond_var_timedwait itself
 * returned an error other than ETIMEDOUT with *ready still false, or
 * _SEL_CONDVAR_READY on a normal wakeup (always, for an infinite wait; also
 * for a deadline wait if *ready is already true by the time either
 * ETIMEDOUT or an unexpected error is observed). Always resets *ready to
 * false before returning.
 *
 * The FAILURE case mirrors every other timed-wait function in this file
 * (circq_timed_send_zc/circq_timed_recv_zc/dynmq_timed_recv_zc), all of
 * which already treat a non-zero, non-ETIMEDOUT return from
 * cond_var_timedwait as ccol_unexpected_failure rather than retrying; this
 * function was the one place in the file that instead kept calling
 * cond_var_timedwait again with the same deadline on such an error (since
 * the ETIMEDOUT check was the loop's only exit condition besides *ready
 * itself), busy-looping forever instead of ever returning to the caller.
 *
 * The FAILURE branch re-checks *ready before committing to FAILURE, mirroring
 * the ETIMEDOUT branch's own identical re-check immediately above it:
 * cond_var_timedwait always re-acquires *sel_mtx before returning, whether it
 * succeeds or fails, so a producer's _notify_waiter (which itself requires
 * *sel_mtx to set *ready = true) can legitimately complete and hand the mutex
 * back to this exact call the instant before an unrelated, spurious
 * non-ETIMEDOUT error is also reported; without this re-check, that genuine,
 * already-delivered wakeup would be silently discarded and reported to the
 * caller as ccol_unexpected_failure instead of the success it actually is. */
static _sel_condvar_outcome _sel_wait_condvar(mutex_t *sel_mtx,
                                              cond_var_t *sel_cond, bool *ready,
                                              bool has_deadline,
                                              const struct timespec *deadline) {
  mutex_lock(*sel_mtx);
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
        wait_ret = cond_var_timedwait(*sel_cond, *sel_mtx, *deadline);
      }
#else
      wait_ret = cond_var_timedwait(*sel_cond, *sel_mtx, *deadline);
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
    mutex_unlock(*sel_mtx);
    return outcome;
  }
  while (!*ready) cond_var_wait(*sel_cond, *sel_mtx);
  *ready = false;
  mutex_unlock(*sel_mtx);
  return _SEL_CONDVAR_READY;
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

  if (ev.data.u64 & _SEL_FD_TAG) {
    int fd = (int)(ev.data.u64 & ~_SEL_FD_TAG);
    deregister_all_sel_waiters(n, nodes, selectables);

    /* One combined registration may represent several selectables sharing
     * this fd (see _sel_setup_epoll); pick the first one, in the caller's
     * own array order, whose own direction is actually satisfied by the
     * events the kernel just reported. Always resolvable: ev.events is
     * necessarily a subset of the combined mask registered for fd, which is
     * itself the OR of every member's own base mask, so any bit set in
     * ev.events came from at least one member's own base mask and matches
     * that member. */
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

    /* Readiness only, for either direction: ccol_select() never reads or
     * writes the fd itself, the caller performs its own read(2)/recv(2) or
     * write(2)/send(2) afterward. */
    *out_retval = ccol_success;
    *ready_index = match;
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
      _sel_condvar_outcome outcome = _sel_wait_condvar(
          &sel_mtx, &sel_cond, &ready, has_deadline, &deadline);
      if (outcome == _SEL_CONDVAR_TIMED_OUT) {
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

/* Internal, mutable registration struct; the PUBLIC event_reg type (see
 * cthreadcomm.h) is an opaque uint64_t handle resolved against this loop's
 * own reg_slots table (see _event_reg_resolve) before ever being
 * dereferenced; never this struct itself. */
typedef struct event_reg_s event_reg_s;
struct event_reg_s {
  ccol_selectable sel;
  event_handlers_t handlers;
  void *arg;
  _Atomic int refcount; /* 1 while registered; +1 per in-flight callback */
  _Atomic bool removed;

  /* True between a successful event_loop_pause and the matching
   * event_loop_resume: the registration stays fully intact (still occupies
   * its slot in owning_entry->as.fd.read_reg/write_reg, still counts toward
   * loop->reg_count, still keeps its generation) but is excluded from the
   * fd's combined epoll interest mask, so no callback fires for it while
   * true. Read by _event_loop_rearm_entry_locked and _event_loop_add_fd's
   * own initial mask computation, both of which already recompute the mask
   * fresh from live state rather than a cached snapshot; this is simply
   * one more bit of live state they read, not a new mechanism. Always false
   * for a queue/channel registration (event_loop_pause rejects those, same
   * restriction as event_loop_modify). */
  _Atomic bool paused;

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
   * written again after that copy; safe to read unconditionally under
   * reg's own deferred-free contract, exactly like stripe_idx. */
  uint64_t generation;

  /* This reg's own index into loop->reg_slots (see struct event_loop_s's
   * own reg_slots field comment for the full design). Set once, at the
   * same point stripe_idx/generation are, and never written again; used
   * by event_loop_remove (to invalidate the slot the instant this reg is
   * removed) and by nothing else, since every OTHER lookup goes through
   * the public event_reg handle value, never through this raw struct. */
  uint32_t self_slot_idx;

  /* Pinned by _event_reg_resolve (mutex-protected increment, under
   * loop->reg_slot_mutex) for as long as some caller holds a just-resolved
   * event_reg_s* it hasn't yet released via _event_reg_resolve_unpin.
   * _event_loop_reclaim_pending_frees will not actually free a reg while
   * this is nonzero, regardless of its refcount; this is what makes
   * freeing a reg concurrently with an in-flight event_loop_modify/
   * _pause/_resume/_remove/event_loop_reg_generation call on that exact
   * reg impossible, closing the gap the old poller_batch_gen-epoch-only
   * scheme left open (see struct event_loop_s's own reg_slots comment). */
  _Atomic size_t pending_resolve_count;

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
  event_reg_s *loop_list_prev;
  event_reg_s *loop_list_next;

  /* Linked into loop->pending_reg_frees when its refcount reaches 0 (see
   * _event_loop_defer_reg_free below); never reused once freeing begins. */
  event_reg_s *pending_free_next;
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
   * epoll_ctl'd), and never written again; every reader (the dispatch
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
   * concurrent epoll_wait call (the classic "thundering herd"); nothing
   * about epoll itself prevents that. Held across the entire dispatch of
   * this entry (both the read_reg/write_reg collection under the stripe
   * lock below AND the callback invocation itself, unlike the stripe lock
   * which is only ever held for collection), this is what actually
   * guarantees a single registration's callback is never invoked
   * concurrently with itself, and, since both directions on one fd share
   * this same entry, that a read and a write registration on the same fd
   * never run concurrently with each other either. This is deliberately a
   * strict guarantee (serializing not just each direction against itself,
   * but both directions against each other too), so a caller layering a
   * single shared resource (e.g. one TLS connection object) across both
   * directions of one fd never needs an ad-hoc lock of its own for that.
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

  /* Snapshot of loop->poller_batch_gen taken at the moment this entry is
   * pushed onto loop->pending_entry_frees (see _event_loop_defer_entry_free).
   * See the large comment above _event_loop_reclaim_pending_frees for the
   * full reclamation scheme this supports. */
  size_t defer_gen;

  /* True once event_loop_remove has fully unregistered this entry (no
   * directions/reg left) and deferred it for freeing; set exactly once,
   * inside _event_loop_defer_entry_free, alongside defer_gen. Consulted by
   * _event_loop_rearm_entry_locked (the shared EPOLLONESHOT re-arm helper)
   * to skip touching a real fd that may already be closed/reused by the
   * application by the time a dispatch job gets around to re-arming it;
   * the same hazard the generation counter exists to guard callers against
   * elsewhere, applied to this module's own internal re-arm call. */
  _Atomic bool removed;

  /* +1 while a dispatch job referencing this entry is queued or executing
   * on a ctpool worker (num_reactor_threads > 1 only; always 0 for the
   * num_reactor_threads == 1 path, which never creates a job in the first
   * place). A job needs entry to stay alive for its own duration (at
   * minimum, to lock/unlock entry->dispatch_lock and to re-arm epoll
   * interest afterward), independently of whether event_loop_remove has
   * already fully unregistered it in the meantime. _event_loop_
   * reclaim_pending_frees will not actually free a deferred entry while
   * this is nonzero, regardless of how far poller_batch_gen has advanced;
   * the epoch check alone only proves the POLLER's own already-fetched
   * batch can no longer reference this entry; it says nothing about a
   * ctpool worker's job, submitted well after collection, still running. */
  _Atomic size_t refcount;

  union {
    struct {
      event_reg_s *read_reg;
      event_reg_s *write_reg;
    } fd;
    event_reg_s *reg; /* queue/channel selectables: 1:1, no sharing */
  } as;

  /* fd selectables only: whether this entry's fd is currently present in
   * loop->epfd's own interest set. EPOLLERR/EPOLLHUP are reported by the
   * kernel unconditionally, regardless of the registered interest mask
   * (even a mask of 0 still gets them), so a fd that has every one of its
   * live directions currently paused (event_loop_pause) can never be fully
   * silenced by narrowing the mask alone: if the fd is (or becomes) an
   * error/hangup condition while paused, level-triggered epoll_wait would
   * otherwise keep reporting it, over and over, with nothing to show for
   * it each time (collection still fires and bumps refcount; only the
   * callback itself is skipped, per reg->paused's own re-check in
   * _event_loop_run_callback); a real, reproduced unbounded CPU-spin
   * busy loop in the poller thread (or, with dispatch_pool, a continuous
   * stream of allocate/submit/dequeue/skip/re-arm cycles), silently
   * contradicting event_loop_pause's own documented "no callback fires,
   * exactly as if it had been removed" contract: a REMOVED registration
   * produces zero further wakeups (EPOLL_CTL_DEL), while a merely
   * mask-narrowed paused one cannot, since ERR/HUP monitoring cannot be
   * opted out of via the mask. Fixed by actually removing the fd from
   * loop->epfd's interest set (EPOLL_CTL_DEL) whenever
   * _event_loop_rearm_entry_locked computes a mask with no real interest
   * bits left (every live direction paused), and re-adding it
   * (EPOLL_CTL_ADD, not MOD, since MOD on a not-currently-registered fd
   * fails with ENOENT) once a direction resumes or a new one registers;
   * this flag is what lets both _event_loop_add_fd and
   * _event_loop_rearm_entry_locked pick the correct epoll_ctl operation.
   * Read and written only while the entry's own stripe lock is held,
   * mirroring every other mutable field on this struct; no atomicity
   * needed. Always false for a queue/channel entry (its bridge eventfd is
   * added once, in _event_loop_add_queue, and never removed/re-added by
   * this mechanism: an eventfd has no analogous "always-on" error
   * condition to worry about, and queue/channel registrations cannot be
   * paused in the first place). */
  bool epoll_added;

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
  chmap fd_index;               /* int fd -> event_entry*; real fds only */
  event_reg_s *queue_regs_head; /* queue/channel-backed regs in this stripe */
} event_loop_stripe_t;

/* event_loop is an opaque value handle (top 32 bits = slot index, bottom 32
 * bits = generation; see include/cthreadcomm.h's own doc comment on the
 * typedef), resolved through this table before the underlying struct
 * event_loop_s* is ever touched. This is what lets __event_loop_destroy
 * detect BOTH a concurrent double-destroy (racing another destroy on the
 * same still-live handle) AND a sequential one (a stale handle, from an
 * earlier, already-completed destroy) as a fatal_err rather than a
 * use-after-free/double-free: a slot is marked not-in-use the instant it is
 * released, and its generation is bumped on every reuse, so a stale handle
 * can never alias a later, unrelated loop occupying the same slot index.
 * Mirrors chttpcli_slot_table/chttpsvr_slot_table exactly; see
 * src/chttpclient.c's own copy of this comment for the full design
 * rationale. */
typedef struct {
  struct event_loop_s *ptr; /* NULL when slot is free */
  uint32_t generation;      /* minted fresh on every acquire; monotonic per
                                slot index, starts at 0 (pre-first-use),
                                becomes 1 on first acquire */
  bool in_use;
} event_loop_slot_t;

static struct {
  mutex_t mutex;
  once_flag_t once;
  cvec slots;        /* cvec of event_loop_slot_t; grows via push_back only,
                         indices permanent once allocated */
  cvec free_indices; /* cvec of uint32_t; LIFO free list, O(1) reuse */
} event_loop_slot_table = {0};

/* Forward declarations: bodies defined further below, once struct
 * event_loop_s itself is declared (they dereference a live loop's own
 * shutdown_lock/reg_slot_mutex/stripes[]); registered by
 * _cthreadcomm_register_atfork_once (see queue_mutex_registry's own doc
 * comment for why this is now ONE merged at_fork() triple covering both
 * event_loop_slot_table's own locks and queue_mutex_registry's own queue
 * mutexes, rather than two independent registrations). */
#if FORK_SAFETY_REQUIRED
static void _cthreadcomm_atfork_prepare(void);
static void _cthreadcomm_atfork_release(void);
static void _cthreadcomm_atfork_child_release(void);
#endif

static void _event_loop_slot_table_init_globals(void) {
  mutex_init(event_loop_slot_table.mutex);
  event_loop_slot_table.slots = cvector_create(sizeof(event_loop_slot_t), NULL);
  if (!event_loop_slot_table.slots)
    fatal_err("event_loop slot table: failed to allocate slots vector");
  event_loop_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!event_loop_slot_table.free_indices)
    fatal_err("event_loop slot table: failed to allocate free-index vector");
}

/* Registers the ONE, shared at_fork() triple covering both
 * event_loop_slot_table's own locks and queue_mutex_registry's own queue
 * mutexes; see queue_mutex_registry's own doc comment for the full
 * rationale (a real, reproduced AB-BA deadlock) for why this must be a
 * single merged registration. Lazily ensures BOTH structures' own plain
 * data (mutex + cvec(s), no atfork wiring) are ready first, via each
 * one's own call_once, regardless of which one's first use triggered this
 * call: a process that only ever uses circular_queue/dynamic_queue, and
 * never touches event_loop at all, must not leave event_loop_slot_table's
 * own mutex/cvecs uninitialised, since the merged prepare/release
 * functions this registers unconditionally walk event_loop_slot_table.
 * fork() duplicates only the calling thread; see _cthreadcomm_atfork_
 * prepare's own doc comment for the full lock-inheritance hazard this
 * closes for BOTH parent() and child(), and _cthreadcomm_atfork_child_
 * release's own doc comment for two further, child-only hazards a plain
 * shared release function cannot address (joining a poller thread that
 * exists only in the parent, and sharing the parent's own live kernel
 * epoll object). */
static void _cthreadcomm_register_atfork_once(void) {
#if FORK_SAFETY_REQUIRED
  /* Both call_once lines below stay inside this guard, not just the actual
   * at_fork() registration: every event_loop-side caller of this function
   * already independently ensures event_loop_slot_table's own readiness via
   * its own adjacent call_once(event_loop_slot_table.once, ...) (see e.g.
   * _event_loop_resolve/_event_loop_handle_slot_acquire below), so this
   * line only actually matters for a queue-only caller
   * (_queue_mutex_registry_add/_remove, which never touch
   * event_loop_slot_table.once directly, relying on the merged atfork
   * handlers below needing it ready). With no atfork machinery to ever walk
   * either registry when this macro is 0, neither registry needs to be
   * ready on this path at all. */
  call_once(queue_mutex_registry.once, _queue_mutex_registry_init_globals);
  call_once(event_loop_slot_table.once, _event_loop_slot_table_init_globals);
  at_fork(_cthreadcomm_atfork_prepare, _cthreadcomm_atfork_release,
          _cthreadcomm_atfork_child_release);
#endif
}

/* Not static: intentionally reachable from other .c files in this library
 * (chttpserver.c) that must guarantee this module's own merged at_fork()
 * triple is registered BEFORE their own, so that pthread_atfork's LIFO
 * prepare-handler ordering makes the CALLER's own prepare handler run
 * FIRST at every future fork() (i.e. before this module's own prepare
 * handler, _cthreadcomm_atfork_prepare, ever gets a chance to lock
 * event_loop_slot_table.mutex or any live event_loop's own locks). Not
 * declared in cthreadcomm.h: this is not part of the public API, only a
 * narrow, deliberate escape hatch for a caller that has already read (and
 * must satisfy) this exact ordering requirement; see chttpserver.c's own
 * call site for the full reasoning and the real, TSan-confirmed deadlock
 * this closes. A caller that never uses this function is entirely
 * unaffected: this module's own lazy, call_once-guarded registration
 * happens exactly as it always has, whenever event_loop/circular_queue/
 * dynamic_queue is first used on its own. */
void _cthreadcomm_ensure_atfork_registered_before_caller(void) {
  call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
}

struct event_loop_s {
  int epfd;
  int shutdown_efd;

  /* Exactly one dedicated thread ever calls epoll_wait on epfd, for every
   * configuration; eliminating the thundering-herd cost multiple threads
   * sharing one epoll instance used to pay (see dispatch_pool's own comment
   * for the mechanism that replaces the throughput multiple polling threads
   * used to provide). num_reactor_threads == 1 additionally never creates
   * dispatch_pool at all: this one thread both polls and runs every
   * callback inline, byte-for-byte the module's original single-thread
   * design (including reclamation timing; see the epoch scheme below). */
  thread_id_t poller_thread;
  size_t num_reactor_threads; /* the original, caller-facing parameter;
                               * total OS thread count for this loop is
                               * always exactly this value: 1 (poller_thread)
                               * plus, when > 1, (num_reactor_threads - 1)
                               * ctpool-owned worker threads. */

  /* NULL when num_reactor_threads == 1. Otherwise, num_reactor_threads - 1
   * worker threads that actually execute dispatch callbacks; poller_thread
   * only ever collects readiness and hands a heap-allocated job to this
   * pool via ctpool_submit (unbounded queue, so submission never blocks the
   * poller), never running a callback itself. Reuses cthreadpool.c's
   * already-independently-tested ctpool rather than hand-rolling a second
   * worker-pool/queue implementation inside this module; cthreadpool.c has
   * no dependency on this header, so there is no circular-dependency risk
   * in the other direction. */
  ctpool dispatch_pool;

  /* Guards only shutdown_started/joined/joined_cv (event_loop_shutdown's
   * one-shot leader/follower coordination). Never touches per-fd state;
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
   * CAS, drained via a single atomic_exchange); loop-wide aggregate
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
  _Atomic(event_reg_s *) pending_reg_frees;

  /* Per-loop generation-tagged slot table for the PUBLIC event_reg handle
   * (a uint64_t value: top 32 bits slot index, bottom 32 bits generation),
   * mirroring event_loop_slot_table's own process-wide design exactly, but
   * scoped to this one loop rather than the whole process (regs already
   * belong to a specific, caller-supplied loop, so there is no need for a
   * process-wide table the way event_loop's own handle (which has no
   * owning object to scope it to) requires one).
   *
   * This exists because a raw event_reg* handed directly to the caller has
   * no way to distinguish "still live" from "already freed" without first
   * dereferencing it, which is itself unsafe once the object might already
   * be gone: event_loop_modify/_pause/_resume/_remove/event_loop_reg_
   * generation all have to know a reg's stripe_idx (at minimum) before any
   * lock-protected liveness check can even run. The pre-existing epoch
   * scheme below (poller_batch_gen) only proves no STALE POLLER BATCH can
   * still reference a reg by the time it is actually freed; it says
   * nothing about a second, genuinely concurrent (or merely later, on any
   * schedule) application thread dereferencing that same reg* via one of
   * those five entry points. Resolving the public handle through this
   * table instead (see _event_reg_resolve/_event_reg_resolve_unpin) closes
   * that gap completely and unconditionally, the same way resolving
   * event_loop's own handle through event_loop_slot_table already does for
   * loop*: a stale or already-removed handle is always detected via a
   * mutex-protected index+generation check, never by touching memory that
   * might already be freed. event_loop_remove marks a reg's slot not-in-use
   * (and bumps its generation) the moment it removes that reg, immediately
   * invalidating every future resolve of that handle value, regardless of
   * whether the underlying event_reg_s has actually been freed yet (see
   * _event_loop_reclaim_pending_frees's own comment for when that happens).
   */
  mutex_t reg_slot_mutex;
  cvec reg_slots;        /* cvec of event_reg_slot_t; grows via push_back
                             only, indices permanent once allocated */
  cvec reg_free_indices; /* cvec of uint32_t; LIFO free list, O(1) reuse */

  /* Deferred-free reclamation (see the large comment above
   * _event_loop_reclaim_pending_frees for the full design). Exactly one
   * thread (poller_thread) ever calls epoll_wait, for every configuration,
   * so "safe to free" reduces to a single monotonic scalar rather than a
   * per-thread array: poller_batch_gen is incremented once by
   * poller_thread at its own between-batches point (right before calling
   * epoll_wait again). A deferred item's own defer_gen field (see
   * event_entry and event_reg) is a snapshot of poller_batch_gen taken at
   * defer time; the item's EPOCH condition for freeing is satisfied once
   * defer_gen < poller_batch_gen, since that means poller_thread has
   * crossed a between-batches point (and therefore fully finished any
   * batch it might have had in flight) since the deferral happened. For
   * event_entry specifically, this is only half the condition: see
   * event_entry.refcount's own comment for the other half, needed because
   * (num_reactor_threads > 1 only) a ctpool worker's job can still be using
   * an entry well after poller_thread has moved on. event_reg has no such
   * second condition; its own pre-existing refcount already fully covers
   * "an in-flight callback still needs this reg," regardless of whether
   * that callback runs inline on poller_thread or on a dispatch_pool
   * worker; only the epoch half changed shape here, not reg's own
   * mechanism. */
  _Atomic size_t poller_batch_gen;

  /* Mints event_loop_reg_generation's caller-visible identity token.
   * Loop-wide, monotonic, starts at 1 (0 is reserved to mean "no reg", see
   * event_loop_reg_generation's own doc comment); incremented once per
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
   * anything deterministic-per-registration; round-robin is simpler than
   * hashing bridge_efd (which does not even exist yet at the point the
   * stripe must be chosen, since it's only created after the stripe lock
   * is taken) and gives strictly better distribution besides. */
  _Atomic size_t next_queue_stripe;

  _Atomic size_t reg_count;

  ccol_memmgmt_procs_t *m_procs;

  /* Pinned by _event_loop_resolve (lock-free atomic increment) for as long
   * as some caller holds a just-resolved struct event_loop_s* it hasn't yet
   * released via _event_loop_resolve_unpin. Unlike chttpcli/chttpsvr's
   * identically-named field, the unpin side here is ALSO a bare atomic
   * decrement, not a lock-protected one: event_loop is lock-striped
   * specifically to keep every hot per-registration call
   * (event_loop_add/_modify/_pause/_resume/_remove) free of any single
   * global lock, and a lock-protected unpin would put exactly that lock back
   * on every one of those calls. __event_loop_destroy instead waits for this
   * to reach 0 by polling (see its own comment), which has no lost-wakeup
   * hazard the way a condvar-based wait would, since polling never depends
   * on a signal actually being delivered. */
  _Atomic size_t pending_resolve_count;

  /* This loop's own public handle value, minted once by
   * _event_loop_handle_slot_acquire and never changed again. Needed because
   * event_readable_fn/event_writable_fn/event_error_fn callbacks must be
   * handed the public event_loop handle as their own `loop` argument (so
   * application code that calls event_loop_modify/_pause/_resume/_add/
   * _remove back from within a callback goes through ordinary resolve/pin
   * like any other caller), not the raw struct event_loop_s* this file uses
   * internally; see _event_loop_run_callback's own two call sites. Plain
   * field, no synchronization needed: written exactly once before this
   * loop's own constructor returns the handle to its caller, and dispatch
   * can only begin once the caller has that handle back (nothing can be
   * registered before then), so no callback can ever observe this field
   * before it holds its final value. */
  event_loop self_handle;

  /* Set to true, exclusively by this process's own CHILD-side fork handler
   * (_event_loop_atfork_child_release), for every event_loop instance
   * still marked in_use at the moment of fork(). fork() duplicates only
   * the calling thread, so poller_thread exists, from this process's own
   * point of view, only as inert, copy-on-write memory: no execution
   * context for it ever existed here, and none ever will again. Once
   * true, _event_loop_shutdown_internal must never call thread_join on
   * poller_thread (undefined behaviour: joining a pthread_t whose target
   * was never created by, and can never be joined by, this process) or
   * write to shutdown_efd (a real, kernel-level eventfd object shared,
   * not copied, across fork(): writing it here would incorrectly wake the
   * PARENT's own still-running poller thread, which is still genuinely
   * using that same object); both must instead be treated as already,
   * trivially done. dispatch_pool needs no equivalent check here: it is a
   * ctpool, and cthreadpool.c's own identical foreign_since_fork field
   * already makes ctpool_shutdown_drain safe to call unconditionally
   * regardless of this flag. Confirmed via a standalone reproduction
   * (fork a process with a live, num_reactor_threads > 1 event_loop, then
   * event_loop_destroy() the inherited handle in the child) to reliably
   * (5/5) SIGSEGV inside glibc's own __pthread_clockjoin_ex, reached via
   * ctpool_shutdown_drain's own worker-thread join loop before
   * cthreadpool.c's own fix; never true for a loop actually created (via
   * event_loop_create_with_mprocs) in this process. Compiled out entirely
   * when FORK_SAFETY_REQUIRED is 0 (see that macro's own doc comment in
   * common.h): every site that would otherwise consult this field instead
   * unconditionally takes the same path it already takes when this field
   * is false. */
#if FORK_SAFETY_REQUIRED
  _Atomic bool foreign_since_fork;
#endif

#ifdef RUNNING_UNIT_TESTS
  /* Test-only: incremented once per completed epoll_wait call on
   * poller_thread, regardless of how many events (if any) that call
   * returned. Lets a test directly detect a busy-spin (this counter racing
   * ahead by a large amount within a short, bounded sampling window)
   * instead of relying on flaky wall-clock/CPU-usage measurement; see
   * event_loop_poller_iterations_for_tests. Kept behind RUNNING_UNIT_TESTS
   * so a production build pays zero cost for it, per this project's own
   * performance-first policy. */
  _Atomic uint64_t poller_iterations_for_tests;
#endif
};

/* ========================================================================== */
/*                         FORK SAFETY (pthread_atfork)                       */
/* ========================================================================== */

#if FORK_SAFETY_REQUIRED
/* fork() duplicates only the calling thread; any lock some OTHER thread held
 * at that instant is inherited by the child in a permanently locked state,
 * since no thread survives in the child that could ever unlock it. Every
 * lock this module could plausibly be holding at an arbitrary instant:
 * event_loop_slot_table.mutex (process-wide, taken by every
 * event_loop_create/_destroy/_add/_remove/_modify/_pause/_resume call via
 * _event_loop_resolve/_event_loop_handle_slot_acquire), each still-live
 * loop's own shutdown_lock/reg_slot_mutex/stripes[].lock, and every live
 * circular_queue's/dynamic_queue's own mutex (queue_mutex_registry, see its
 * own doc comment above). All of it is therefore taken here, in prepare(),
 * before fork() is allowed to proceed (so fork() only ever completes once no
 * thread is transiently holding one of them), and released again in both
 * parent() and child() via the same function: every mutex in this module
 * uses the default ("normal") pthread mutex type, which does no owner/TID
 * tracking on Linux glibc, so a plain pthread_mutex_unlock is well-defined
 * even when called by a thread other than whichever one originally locked
 * it (which, for anything the forking thread itself did not hold, no
 * longer exists in the child at all). Contrast clogger.c's own atfork
 * history, where the analogous fix for its rwlock needed a real reinit in
 * the child rather than a plain unlock, specifically because glibc's
 * rwlock write-lock tracks ownership by TID; that hazard does not apply to
 * a plain mutex.
 *
 * Confirmed as a real, reproducible hang via two standalone repros before
 * the event_loop-only version of this fix, not assumed: a thread
 * continuously creating/destroying unrelated event_loop instances raced
 * against repeated fork() calls left roughly 1 in 1000 forked children
 * permanently hung the moment they tried their own, brand-new
 * event_loop_create_with_mprocs call (event_loop_slot_table.mutex
 * inherited already locked); a single, continuously-busy, multi-threaded
 * reactor (mirroring chttpserver.c's/chttpclient.c's own long-lived,
 * process-wide reactors) forked while busy left roughly half of all forked
 * children hung the moment they tried one more event_loop_add on the very
 * loop they had just inherited (a stripe's own lock, coincidentally shared
 * with the fd the vanished poller/dispatch threads were still working on,
 * inherited locked).
 *
 * Deliberately does NOT extend to individual event_entry.dispatch_lock
 * instances. Discovering every live entry can only be done safely by
 * walking a stripe's own fd_index/queue_regs_head under that exact
 * stripe's lock; acquiring an entry's dispatch_lock while still holding
 * that same stripe lock would be the exact reverse of the one and only
 * other ordering this file ever uses between the two (see
 * event_entry.dispatch_lock's own field comment: dispatch_lock is always
 * acquired first and released before a stripe lock is ever considered,
 * never the other way around), so doing it here would introduce a genuine
 * new ABBA deadlock against an ordinary reactor thread inside
 * _event_loop_handle_event (dispatch_lock held, stripe lock wanted)
 * instead of fixing anything. A forked child never has any reactor/
 * dispatch thread of its own for a loop it merely inherited (fork()
 * duplicates only the calling thread), so nothing in the child can ever
 * legitimately dispatch through that loop again regardless of this gap;
 * the one thing it leaves unprotected is a later event_loop_destroy() of
 * that exact inherited loop, in the child, racing a dispatch_lock some
 * other, by-then-vanished parent-side thread happened to hold at fork
 * time (mutex_destroy on a still-locked mutex is itself undefined
 * behaviour per POSIX, independent of fork). Narrower and far less likely
 * than the two hazards above, both demonstrated at 1-in-1000 and
 * 1-in-2 rates respectively; recorded here rather than silently left
 * unfixed, since closing it safely would require restructuring how
 * live entries are discovered (e.g. a dedicated, separately-locked
 * live-entry list, mirroring clogger.c's own live_shareds), a
 * materially larger change than this fix's own demonstrated scope.
 *
 * DOES extend to every queue/channel-backed event_reg_s's own wait_mtx,
 * unlike dispatch_lock above, AND to that reg's own underlying queue's
 * cq->mutex/dq->mutex: the two must be locked in exactly this relative
 * order, matching every real nested-locking pattern found elsewhere in
 * this file:
 *
 *   1. Every live loop's own shutdown_lock, reg_slot_mutex, and every
 *      stripe's own lock (matching _event_loop_add_queue's/
 *      _event_loop_remove_unlink's own stripe->lock-then-cq->mutex
 *      nesting: both are called with the owning stripe's lock already
 *      held, so that lock must already be held here before any of that
 *      stripe's own queue-backed registrations' underlying queue mutex is
 *      ever touched).
 *   2. Every live queue's own mutex (queue_mutex_registry), exactly once
 *      each regardless of how many event_loop registrations (across one
 *      or more stripes/loops, or none at all) reference it: the registry
 *      itself never lists an address twice, so a single pass over it,
 *      done as this self-contained phase, cannot double-lock anything.
 *   3. Every queue-backed registration's own wait_mtx (matching
 *      _notify_waiter's own cq->mutex-then-wait_mtx nesting: it is invoked
 *      from any ordinary producer/consumer thread doing a plain send/recv
 *      on a queue this registration watches, entirely independent of this
 *      loop's own reactor/dispatch threads, always with that queue's own
 *      mutex already held).
 *
 * An EARLIER version of this fix locked every queue's own mutex via a
 * SEPARATE, independent at_fork() registration instead of this single
 * merged one; that was a real, reproduced deadlock, not merely a
 * theoretical concern (confirmed via gdb on a hung standalone repro): with
 * a circular_queue created before the first event_loop in the process
 * (registering that separate at_fork() triple first), pthread_atfork's
 * prepare handlers run in REVERSE registration order, so event_loop's own
 * prepare ran FIRST, locking a queue-backed registration's wait_mtx,
 * and the queue registry's own (separate) prepare ran SECOND, trying to
 * lock that SAME queue's cq->mutex, which a genuinely concurrent
 * circq_send_zc call already held while itself blocked inside
 * _notify_waiter wanting that same wait_mtx: a textbook AB-BA cycle, with
 * the forking thread deadlocked inside fork() itself. The three-phase
 * design above closes this completely by construction: there is exactly
 * ONE at_fork() registration, so there is no "which of two independent
 * handler sets happens to run first" accident left to depend on, and its
 * own three phases are ordered to match constraint 1 and constraint 3
 * above simultaneously for every queue-backed registration, regardless of
 * which stripe/loop it belongs to or whether its queue has an event_loop
 * registration at all.
 *
 * Only ever walks slots with in_use == true, mirroring the exact
 * condition _event_loop_resolve itself already trusts as the sole
 * indicator that slot->ptr is safe to dereference: __event_loop_destroy
 * clears in_use (under this same event_loop_slot_table.mutex) BEFORE
 * doing any of its own, possibly slow, teardown work (joining the poller
 * thread, draining dispatch_pool, freeing every entry/registration), and
 * only actually frees the loop struct itself, then finally clears
 * slot->ptr, well after that teardown has completed; a loop already past
 * that first step is therefore, by this file's own established
 * convention, already off-limits for any purpose, fork-related or not,
 * for the remainder of its teardown, not a new gap this fix
 * introduces. */
static void _cthreadcomm_atfork_prepare(void) {
  mutex_lock(queue_mutex_registry.mutex);
  mutex_lock(event_loop_slot_table.mutex);

  size_t n_loops = cvector_elem_count(event_loop_slot_table.slots);

  /* Phase 1: every live loop's own shutdown_lock/reg_slot_mutex/stripe
   * locks. Deliberately does NOT touch wait_mtx or any queue's own mutex
   * yet; see this function's own doc comment for why those must wait for
   * phases 2 and 3. */
  for (size_t i = 0; i < n_loops; i++) {
    event_loop_slot_t *slot =
        (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct event_loop_s *loop = slot->ptr;
    mutex_lock(loop->shutdown_lock);
    mutex_lock(loop->reg_slot_mutex);
    for (size_t s = 0; s < loop->num_stripes; s++) {
      mutex_lock(loop->stripes[s].lock);
    }
  }

  /* Phase 2: every live queue's own mutex, exactly once each, now that
   * every stripe lock (phase 1) is already held. */
  size_t n_addrs = cvector_elem_count(queue_mutex_registry.addrs);
  for (size_t k = 0; k < n_addrs; k++) {
    mutex_t *m = *(mutex_t **)cvector_at(queue_mutex_registry.addrs, k);
    mutex_lock(*m);
  }

  /* Phase 3: every queue-backed registration's own wait_mtx, now that
   * every queue's own mutex (phase 2) is already held. */
  for (size_t i = 0; i < n_loops; i++) {
    event_loop_slot_t *slot =
        (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct event_loop_s *loop = slot->ptr;
    for (size_t s = 0; s < loop->num_stripes; s++) {
      for (event_reg_s *reg = loop->stripes[s].queue_regs_head; reg;
           reg = reg->loop_list_next) {
        mutex_lock(reg->wait_mtx);
      }
    }
  }
}

/* Shared by both parent() and child(); see _cthreadcomm_atfork_prepare's own
 * doc comment for why a plain unlock (not a reinit) is correct in both
 * branches for this module's mutexes, and for the three-phase design this
 * mirrors (in reverse phase order, though release order does not actually
 * matter for correctness with this module's plain, non-recursive mutexes:
 * unlocking is never itself a blocking operation that could deadlock).
 * Safe to re-walk the identical structure prepare() just walked and
 * release every lock: nothing could have mutated the slot table, the
 * queue registry, or any live loop's own stripe count or registration
 * list in between, since every lock that would be needed to do so is
 * still held at this exact point.
 *
 * is_child additionally, for every still-live loop:
 *  - marks it foreign_since_fork (see that field's own comment) so a
 *    later event_loop_shutdown/_destroy call in THIS process never joins
 *    poller_thread or signals shutdown_efd;
 *  - resets its own pending_resolve_count to 0, for the identical reason
 *    _ctpool_atfork_release_impl's own is_child branch does (a now-
 *    vanished parent-side thread may have left it permanently nonzero
 *    from this process's own point of view; see that function's own
 *    comment for the full reasoning, which applies here unchanged);
 *  - replaces this process's own local epfd with a brand new, empty
 *    epoll instance. loop->epfd (and loop->shutdown_efd's registration
 *    inside it) is a real, kernel-level object duplicated, not
 *    deep-copied, by fork(): this process's own fd number for it still
 *    refers to the SAME underlying epoll instance the still-running
 *    parent's own poller thread continues to call epoll_wait on. Left
 *    as-is, any future epoll_ctl call this process makes through
 *    event_loop_add/_remove/_modify/_pause/_resume (all otherwise still
 *    fully functional bookkeeping operations on this loop's own,
 *    private, copy-on-write registry) would silently mutate the
 *    PARENT's live interest set: an ADD for a genuinely new fd this
 *    process registers could eventually hand the parent's poller thread
 *    an epoll_event whose ev.data.ptr is an event_entry* that only makes
 *    sense in this process's own (already-diverged) heap; a DEL for an
 *    inherited registration would rip the parent's own, still-wanted
 *    interest in that fd out from under it. Swapping this process's own
 *    epfd decouples every future epoll_ctl call this process makes from
 *    the parent's kernel object entirely, at zero cost to this process's
 *    own correctness: with no poller thread of its own surviving the
 *    fork, this process was never going to epoll_wait on it for a real
 *    dispatch anyway (see event_loop_add's own "nothing in the child can
 *    ever legitimately dispatch through that loop again" doc comment). A
 *    subsequent event_loop_remove()/event_loop_destroy() of an inherited
 *    (pre-fork) registration then simply finds nothing to EPOLL_CTL_DEL
 *    on this new, empty instance (silently tolerated: every such
 *    epoll_ctl call's return value is already ignored throughout this
 *    file), which is exactly the point: it must not find, and must not
 *    touch, the parent's own still-live registration for that same fd.
 *    A failure to create the replacement instance is tolerated silently
 *    (loop->epfd is simply left as the original, still-shared one in
 *    that rare case) rather than aborting an otherwise-successful
 *    fork(); nothing else this function does depends on it having
 *    succeeded. */
static void _cthreadcomm_atfork_release_impl(bool is_child) {
  size_t n_loops = cvector_elem_count(event_loop_slot_table.slots);

  for (size_t i = 0; i < n_loops; i++) {
    event_loop_slot_t *slot =
        (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct event_loop_s *loop = slot->ptr;

    if (is_child) {
      atomic_store(&loop->foreign_since_fork, true);
      atomic_store(&loop->pending_resolve_count, (size_t)0);

      int old_epfd = loop->epfd;
      int new_epfd = epoll_create1(EPOLL_CLOEXEC);
      if (new_epfd >= 0) {
        loop->epfd = new_epfd;
        close(old_epfd);
      }
    }

    for (size_t s = 0; s < loop->num_stripes; s++) {
      for (event_reg_s *reg = loop->stripes[s].queue_regs_head; reg;
           reg = reg->loop_list_next) {
        mutex_unlock(reg->wait_mtx);
      }
    }
  }

  /* Every live queue's own mutex, exactly once each: unlocked here, in one
   * self-contained pass, mirroring phase 2 of prepare() exactly; not
   * interleaved into the loop above, since a queue's mutex may be shared
   * by registrations across more than one stripe/loop, and unlocking it
   * more than once would be undefined behaviour for this module's plain
   * mutexes. */
  size_t n_addrs = cvector_elem_count(queue_mutex_registry.addrs);
  for (size_t k = 0; k < n_addrs; k++) {
    mutex_t *m = *(mutex_t **)cvector_at(queue_mutex_registry.addrs, k);
    mutex_unlock(*m);
  }

  for (size_t i = 0; i < n_loops; i++) {
    event_loop_slot_t *slot =
        (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct event_loop_s *loop = slot->ptr;
    for (size_t s = 0; s < loop->num_stripes; s++) {
      mutex_unlock(loop->stripes[s].lock);
    }
    mutex_unlock(loop->reg_slot_mutex);
    mutex_unlock(loop->shutdown_lock);
  }

  mutex_unlock(event_loop_slot_table.mutex);
  mutex_unlock(queue_mutex_registry.mutex);
}

static void _cthreadcomm_atfork_release(void) {
  _cthreadcomm_atfork_release_impl(false);
}

/* Child-side counterpart to _cthreadcomm_atfork_release; see
 * _cthreadcomm_atfork_release_impl's own comment for exactly what the
 * extra, child-only work is and why each part is needed. Must run before
 * any application code in this process can possibly reach one of these
 * loops' own shutdown/destroy/add/remove/modify path: pthread_atfork's
 * child handler runs synchronously, as part of fork() itself returning,
 * strictly before fork()'s return value ever reaches the calling code. */
static void _cthreadcomm_atfork_child_release(void) {
  _cthreadcomm_atfork_release_impl(true);
}
#endif /* FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                    EVENT_LOOP HANDLE RESOLVE / UNPIN                       */
/* ========================================================================== */

/* Resolves h and pins the result against concurrent destroy, or returns NULL
 * if h is 0, garbage, or references a currently-free or already-reused
 * (wrong-generation) slot. On success, the caller MUST call
 * _event_loop_resolve_unpin(result) exactly once, as soon as it is done
 * touching the resolved struct event_loop_s*. */
static struct event_loop_s *_event_loop_resolve(event_loop h) {
  call_once(event_loop_slot_table.once, _event_loop_slot_table_init_globals);
  call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  mutex_lock(event_loop_slot_table.mutex);
  struct event_loop_s *raw = NULL;
  if (idx < cvector_elem_count(event_loop_slot_table.slots)) {
    event_loop_slot_t *slot =
        (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* Lock-free: no raw-level lock acquisition here at all, matching this
   * loop's own field comment on pending_resolve_count; event_loop's own
   * lock striping exists specifically to keep every hot per-registration
   * call free of any single global lock, and this resolve step must not
   * reintroduce one. Safe because raw is guaranteed still-allocated here
   * regardless: the only thing that could make it unsafe to touch,
   * __event_loop_destroy's slot-release step, also requires
   * event_loop_slot_table.mutex, which we still hold at this exact point. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  mutex_unlock(event_loop_slot_table.mutex);
  return raw;
}

static void _event_loop_resolve_unpin(struct event_loop_s *raw) {
  /* Bare atomic decrement, no lock, no broadcast; see
   * pending_resolve_count's own field comment for why this asymmetry
   * (unlike chttpcli/chttpsvr's lock-protected decrement) is correct here:
   * __event_loop_destroy waits for this to reach 0 by polling, not by
   * sleeping on a condvar, so there is no lost-wakeup hazard to guard
   * against and nothing to broadcast to. */
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
}

/* Allocates a fresh slot (or reuses a freed one) for loop and returns the
 * resulting handle, or 0 on OOM. Called once, from
 * event_loop_create_with_mprocs, after the loop is otherwise fully
 * constructed (including its poller thread and, if configured, its
 * dispatch_pool; see that function's own comment on why a slot-acquire
 * failure at this point must stop them rather than merely free memory). */
static event_loop _event_loop_handle_slot_acquire(struct event_loop_s *loop) {
  call_once(event_loop_slot_table.once, _event_loop_slot_table_init_globals);
  call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  mutex_lock(event_loop_slot_table.mutex);
  uint32_t idx;
  event_loop_slot_t *slot;
  if (cvector_elem_count(event_loop_slot_table.free_indices) > 0) {
    cvector_pop_back(event_loop_slot_table.free_indices, &idx);
    slot = (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, idx);
  } else {
    event_loop_slot_t fresh = {0};
    if (cvector_push_back(event_loop_slot_table.slots, &fresh) !=
        ccol_success) {
      mutex_unlock(event_loop_slot_table.mutex);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(event_loop_slot_table.slots) - 1;
    slot = (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that would collide with the reserved
   * "invalid handle" sentinel (0) after ~2^32 reuses of this exact slot
   * index; see chttpcli_handle_slot_acquire's identical guard for the full
   * rationale. */
  if (slot->generation == 0) slot->generation++;
  slot->ptr = loop;
  slot->in_use = true;
  event_loop h = ((event_loop)idx << 32) | (event_loop)slot->generation;
  mutex_unlock(event_loop_slot_table.mutex);
  return h;
}

/* Multiplicative hash (Knuth's constant, in the same spirit as chashmap's
 * own documented Fibonacci hashing for open addressing) reduced mod
 * num_stripes. Plain fd % num_stripes is deliberately avoided: fds are
 * small, kernel-sequential integers, and a plain modulo risks clustering
 * (e.g. an all-even-fd pattern landing in only half the stripes when
 * num_stripes is a power of two). fd selectables only; queue/channel
 * selectables are assigned via loop->next_queue_stripe instead (see
 * event_loop_add), since their stripe has no consistency requirement to
 * satisfy in the first place. */
static size_t _stripe_index_for_fd(struct event_loop_s *loop, int fd) {
  uint32_t h = (uint32_t)fd * 2654435761u;
  return (size_t)h % loop->num_stripes;
}

/* fd_index is keyed ccol_int -> ccol_pointer, both integral and <= 8 bytes,
 * so chashmap's own should_use_open_addressing() dispatches it to the
 * open-addressing backend (its oa_slot storage is _Alignas(max_align_t)),
 * not separate chaining; this memcpy is defense-in-depth rather than a
 * live alignment requirement either way, kept for consistency with
 * cjson/cyaml/clrucache/chttpclient's own chmap-backed pointer storage
 * (which use separate chaining, and where the packed chmap_entry SSO
 * union genuinely does require it), and because a caller of this map has
 * no business assuming one particular backend forever.
 *
 * These all operate on one stripe's own fd_index/queue_regs_head, passed in
 * directly by the caller (which has already computed the right stripe via
 * _stripe_index_for_fd or the round-robin counter and locked it); not on
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

static void _loop_queue_list_add(event_loop_stripe_t *stripe,
                                 event_reg_s *reg) {
  reg->loop_list_prev = NULL;
  reg->loop_list_next = stripe->queue_regs_head;
  if (stripe->queue_regs_head) stripe->queue_regs_head->loop_list_prev = reg;
  stripe->queue_regs_head = reg;
}

static void _loop_queue_list_remove(event_loop_stripe_t *stripe,
                                    event_reg_s *reg) {
  if (reg->loop_list_prev)
    reg->loop_list_prev->loop_list_next = reg->loop_list_next;
  else
    stripe->queue_regs_head = reg->loop_list_next;
  if (reg->loop_list_next)
    reg->loop_list_next->loop_list_prev = reg->loop_list_prev;
}

/* Resolves the queue mutex and the correct sel_{read,write}_waiters_head/
 * sel_{read,write}_rotor pointers for sel (circq or dynq only; fd
 * selectables never reach here). ccol_selectable_from_chan has already
 * resolved chan selectables down to a concrete circq by the time sel
 * reaches event_loop_add. */
static void _queue_sel_locate(ccol_selectable *sel, mutex_t **out_mtx,
                              ccol_sel_waiter ***out_head,
                              ccol_sel_waiter ***out_rotor) {
  if (sel->type == ccol_selectable_circq) {
    circular_queue *cq = sel->cq;
    *out_mtx = &cq->mutex;
    *out_head = (sel->dir == ccol_select_read) ? &cq->sel_read_waiters_head
                                               : &cq->sel_write_waiters_head;
    *out_rotor = (sel->dir == ccol_select_read) ? &cq->sel_read_rotor
                                                : &cq->sel_write_rotor;
  } else {
    dynamic_queue *dq = sel->dq;
    *out_mtx = &dq->mutex;
    *out_head = (sel->dir == ccol_select_read) ? &dq->sel_read_waiters_head
                                               : &dq->sel_write_waiters_head;
    *out_rotor = (sel->dir == ccol_select_read) ? &dq->sel_read_rotor
                                                : &dq->sel_write_rotor;
  }
}

/* Per-loop slot table entry backing the public event_reg handle value; see
 * struct event_loop_s's own reg_slots field comment for the full design. */
typedef struct {
  event_reg_s *ptr;    /* NULL when slot is free */
  uint32_t generation; /* minted fresh on every acquire; monotonic per
                           slot index, starts at 0 (pre-first-use),
                           becomes 1 on first acquire */
  bool in_use;
} event_reg_slot_t;

#ifdef RUNNING_UNIT_TESTS
/* Test-only: forces the very next _event_reg_slot_acquire call to report
 * failure (as if loop->reg_slots's own growth allocation had failed),
 * regardless of which allocator loop actually uses, then auto-disarms. No
 * public constructor's own allocator parameter can reliably force a
 * failure at exactly this one call: loop->reg_slots/reg_free_indices are
 * both pre-allocated to a nonzero minimum capacity at loop-creation time
 * (see cvector's own "min capacity 4" policy), so an ordinary
 * event_loop_add call only ever reaches a real growth-triggering
 * reallocation once a loop already holds several live registrations, and
 * even then the SAME per-loop allocator is shared by every other
 * allocation this module makes (the fd registry chmap, dispatch_pool,
 * ...), making it impractical to fail this one call in isolation via fault
 * injection alone. Mirrors this codebase's own established precedent for
 * exactly this class of hard-to-reach allocation-failure test (see e.g.
 * clogger.c's clog_test_force_next_fresh_slot_registration_failure). */
static _Atomic bool g_force_next_reg_slot_acquire_failure = false;

/* Snapshot of loop->reg_count taken at the exact moment a forced failure
 * above fires, before returning to the caller. This is what lets a test
 * directly and deterministically prove event_loop_add's own ORDERING (slot
 * acquired before reg is ever wired into the registry, not after), rather
 * than only being able to observe the two orderings' identical end state
 * (event_loop_add returning EVENT_REG_INVALID with reg_count back at 0
 * either way, since a post-wiring failure's own rollback also restores
 * reg_count): reg_count can only have already been incremented for THIS
 * call's own registration if wiring ran before slot acquisition was ever
 * attempted, so a snapshot of 0 here proves wiring had not yet happened.
 * Deliberately not reset between calls (unlike the one-shot force flag
 * above): a test that never armed the force flag simply never reads this
 * accessor. */
static _Atomic size_t g_forced_slot_acquire_failure_reg_count_snapshot = 0;

void event_loop_test_force_next_reg_slot_acquire_failure(void) {
  atomic_store(&g_force_next_reg_slot_acquire_failure, true);
}

size_t event_loop_test_last_forced_slot_acquire_failure_reg_count(void) {
  return atomic_load(&g_forced_slot_acquire_failure_reg_count_snapshot);
}
#endif

/* Allocates a fresh slot (or reuses a freed one) in loop->reg_slots for reg
 * and returns the resulting event_reg handle, or 0 on OOM. Called from
 * event_loop_add BEFORE reg is wired into the fd/queue registry (see that
 * function's own comment for why this ordering, not the reverse, is what
 * keeps a slot-acquire failure cheap to unwind: unlike event_loop's own
 * constructor, where a slot-acquire failure has to unwind an
 * already-running poller thread, a reg that has not yet been wired into
 * any registry has nothing else to unwind). Sets reg->self_slot_idx.
 * Caller must not hold loop->reg_slot_mutex. */
static event_reg _event_reg_slot_acquire(struct event_loop_s *loop,
                                         event_reg_s *reg) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_force_next_reg_slot_acquire_failure)) {
    atomic_store(&g_force_next_reg_slot_acquire_failure, false);
    atomic_store(&g_forced_slot_acquire_failure_reg_count_snapshot,
                 atomic_load(&loop->reg_count));
    return 0;
  }
#endif
  mutex_lock(loop->reg_slot_mutex);
  uint32_t idx;
  event_reg_slot_t *slot;
  if (cvector_elem_count(loop->reg_free_indices) > 0) {
    cvector_pop_back(loop->reg_free_indices, &idx);
    slot = (event_reg_slot_t *)cvector_at(loop->reg_slots, idx);
  } else {
    event_reg_slot_t fresh = {0};
    if (cvector_push_back(loop->reg_slots, &fresh) != ccol_success) {
      mutex_unlock(loop->reg_slot_mutex);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(loop->reg_slots) - 1;
    slot = (event_reg_slot_t *)cvector_at(loop->reg_slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that would collide with the reserved
   * "invalid handle" sentinel (0) after ~2^32 reuses of this exact slot
   * index; see _event_loop_handle_slot_acquire's identical guard for the
   * full rationale. */
  if (slot->generation == 0) slot->generation++;
  slot->ptr = reg;
  slot->in_use = true;
  reg->self_slot_idx = idx;
  event_reg h = ((event_reg)idx << 32) | (event_reg)slot->generation;
  mutex_unlock(loop->reg_slot_mutex);
  return h;
}

/* Resolves h against loop's own reg slot table and pins the result against
 * a concurrent free, or returns NULL if h is 0, garbage, or references a
 * currently-free or already-reused (wrong-generation) slot. On success,
 * the caller MUST call _event_reg_resolve_unpin(result) exactly once, as
 * soon as it is done touching the resolved event_reg_s*. Mirrors
 * _event_loop_resolve exactly, scoped to one loop's own table instead of
 * the process-wide one. */
static event_reg_s *_event_reg_resolve(struct event_loop_s *loop, event_reg h) {
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  mutex_lock(loop->reg_slot_mutex);
  event_reg_s *raw = NULL;
  if (idx < cvector_elem_count(loop->reg_slots)) {
    event_reg_slot_t *slot =
        (event_reg_slot_t *)cvector_at(loop->reg_slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* The increment happens while STILL holding reg_slot_mutex, exactly like
   * _event_loop_resolve's own pending_resolve_count increment: the only
   * thing that could make touching raw unsafe here is the slot having
   * already been marked not-in-use (event_loop_remove, under this same
   * mutex), which we have just confirmed is not the case. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  mutex_unlock(loop->reg_slot_mutex);
  return raw;
}

static void _event_reg_resolve_unpin(event_reg_s *raw) {
  /* Bare atomic decrement, no lock: _event_loop_reclaim_pending_frees polls
   * this value rather than waiting on a condvar, so there is no
   * lost-wakeup hazard to guard against; mirrors
   * _event_loop_resolve_unpin's identical reasoning exactly. */
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
}

/* Marks reg's own slot not-in-use and bumps its generation, immediately
 * invalidating every future _event_reg_resolve of the handle value
 * event_loop_add returned for it, and pushes the index onto the free list
 * for a future reg to reuse. Called from event_loop_remove at the exact
 * point reg is logically removed, independent of (and always well before)
 * whenever the underlying event_reg_s struct itself is actually freed (see
 * _event_loop_reclaim_pending_frees): the slot and the struct have
 * separate lifecycles, linked only by reg->self_slot_idx, so reusing this
 * slot index for a brand new registration the moment this function returns
 * can never race the original reg's own still-pending deferred free. */
static void _event_reg_slot_release(struct event_loop_s *loop,
                                    event_reg_s *reg) {
  mutex_lock(loop->reg_slot_mutex);
  event_reg_slot_t *slot =
      (event_reg_slot_t *)cvector_at(loop->reg_slots, reg->self_slot_idx);
  slot->in_use = false;
  slot->ptr = NULL;
  cvector_push_back(loop->reg_free_indices, &reg->self_slot_idx);
  mutex_unlock(loop->reg_slot_mutex);
}

static event_reg_s *_event_reg_create(struct event_loop_s *loop,
                                      ccol_selectable sel,
                                      event_handlers_t handlers, void *arg) {
  event_reg_s *reg = _mem_calloc(loop->m_procs, 1, sizeof(event_reg_s));
  if (!reg) return NULL;
  reg->sel = sel;
  reg->handlers = handlers;
  reg->arg = arg;
  atomic_init(&reg->refcount, 1);
  atomic_init(&reg->removed, false);
  atomic_init(&reg->paused, false);
  atomic_init(&reg->pending_resolve_count, (size_t)0);
  reg->bridge_efd = -1;
  /* wait_mtx/wait_cond are initialized here, unconditionally for every
   * non-fd (queue/channel) registration, exactly once, regardless of
   * whether _event_loop_add_queue ever actually runs to completion
   * (event_loop_add's own event_entry allocation can fail and
   * short-circuit before _event_loop_add_queue is ever called). This is
   * what lets _event_reg_free be the SOLE owner of destroying them, on
   * every path (a later, successful removal, or any registration-time
   * failure) without risking a double pthread_mutex_destroy/
   * pthread_cond_destroy (undefined behaviour per POSIX) or a destroy of
   * never-initialized memory; see _event_reg_free's own comment. */
  if (sel.type != ccol_selectable_fd) {
    mutex_init(reg->wait_mtx);
    cond_var_init(reg->wait_cond);
  }
  return reg;
}

/* The single owner of tearing down reg's wait_mtx/wait_cond (initialized by
 * _event_reg_create, above) and closing its bridge_efd (opened by
 * _event_loop_add_queue): called both when a live registration is finally
 * freed (after removal, once deferred-free reclamation allows it) and when
 * event_loop_add itself fails partway through registering a queue/channel
 * selectable. Neither _event_loop_add_queue's own failure paths nor any
 * other function may destroy these fields; doing so here, exactly once
 * regardless of which caller reaches this function, is what prevents the
 * double-destroy that resulted from _event_loop_add_queue tearing them down
 * on its own failure paths AND this function tearing them down again right
 * after. */
static void _event_reg_free(struct event_loop_s *loop, event_reg_s *reg) {
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
                                        event_reg_s *reg) {
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
    atomic_init(&entry->removed, false);
    atomic_init(&entry->refcount, (size_t)0);
    /* Minted once per NEW entry, never for a second direction joining an
     * already-registered fd (that case takes the !new_entry path below and
     * shares the existing entry's generation, correctly reflecting that
     * both directions represent one logical connection). */
    entry->generation = atomic_fetch_add(&loop->fd_generation_counter, 1) + 1;
  }

  event_reg_s **slot = (reg->sel.dir == ccol_select_read)
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

  /* EPOLLONESHOT only when dispatch_pool exists (num_reactor_threads > 1):
   * collection (poller_thread) and dispatch (a ctpool worker) are decoupled
   * in that configuration, so a still-ready, not-yet-dispatched fd would
   * otherwise be re-observed by poller_thread on every subsequent
   * epoll_wait call for as long as the backlog persists, minting an
   * unbounded stream of redundant dispatch jobs; a real resource-
   * exhaustion/livelock risk under sustained load, not a rare corner case.
   * _event_loop_rearm_entry_locked re-arms after each dispatch job. MUST
   * NOT be set for num_reactor_threads == 1: that path's dispatch
   * (_event_loop_handle_event) is unchanged from before this redesign and
   * never re-arms anything, since collection and dispatch are still one
   * synchronous call on the same thread with nothing else to re-arm it;
   * setting EPOLLONESHOT there would silently stop delivering any event
   * for this fd after the first one. */
  uint32_t mask = loop->dispatch_pool ? EPOLLONESHOT : 0;
  if (entry->as.fd.read_reg && !atomic_load(&entry->as.fd.read_reg->paused))
    mask |= (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
  if (entry->as.fd.write_reg && !atomic_load(&entry->as.fd.write_reg->paused))
    mask |= (EPOLLOUT | EPOLLERR | EPOLLHUP);

  struct epoll_event ev;
  ev.data.ptr = entry;
  ev.events = mask;
  /* Keyed off entry->epoll_added (whether the fd is CURRENTLY present in
   * loop->epfd's interest set), not new_entry: an existing entry may have
   * been EPOLL_CTL_DEL'd by a prior _event_loop_rearm_entry_locked call
   * because every one of its then-live directions was paused (see
   * event_entry.epoll_added's own field comment); a second direction
   * joining it in that state needs EPOLL_CTL_ADD, not MOD (MOD on a fd not
   * currently registered fails with ENOENT). The reg just added to *slot
   * above is always freshly non-paused (see _event_reg_create), so mask
   * always carries real interest bits here regardless of any sibling
   * direction's own paused state. */
  int ctl_op = entry->epoll_added ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if (epoll_ctl(loop->epfd, ctl_op, fd, &ev) < 0) {
    *slot = NULL;
    if (new_entry) {
      mutex_destroy(entry->dispatch_lock);
      _mem_free(loop->m_procs, entry);
    }
    return ccol_unexpected_failure;
  }
  entry->epoll_added = true;

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

/* Peeks whether sel (a resolved circq/dynq selectable; never fd) is
 * currently ready for its own direction, using the single, shared
 * definition of "ready" for a queue selectable in this file: msg_count > 0
 * for read; room to send (and sending enabled) for write. Must be called
 * with the resolved queue's own mutex already held (mirrors every other
 * reader of msg_count/max_size/writing_disabled in this file). Shared by
 * _event_loop_add_queue's own already_ready check and
 * _event_loop_queue_cascade_notify_next below, so the two can never
 * independently drift out of sync with each other about what "ready"
 * means. */
static bool _queue_sel_is_ready(ccol_selectable *sel) {
  if (sel->type == ccol_selectable_circq) {
    circular_queue *cq = sel->cq;
    return (sel->dir == ccol_select_read)
               ? cq->msg_count > 0
               : (cq->msg_count < cq->max_size && !cq->writing_disabled);
  }
  dynamic_queue *dq = sel->dq;
  return (sel->dir == ccol_select_read)
             ? dq->msg_count > 0
             : (!dq->writing_disabled && dq->msg_count < max_elem_count);
}

/* Sets up a queue-backed registration: allocates the dedicated bridge
 * eventfd, registers it with the loop's persistent epoll instance, and
 * links reg's embedded waiter_node into the queue's own waiter list,
 * permanently (unlike ccol_select's transient per-call nodes), so the
 * queue's existing notify_one_sel_waiter/notify_all_sel_waiters (already
 * called from circq_send_zc/recv_zc, dynmq_send_zc/recv_zc, and the
 * enable_sending/disable_sending functions) wakes this registration too. */
static ccol_retval_t _event_loop_add_queue(struct event_loop_s *loop,
                                           event_entry *entry,
                                           event_reg_s *reg) {
  reg->bridge_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (reg->bridge_efd < 0) {
    /* reg->wait_mtx/wait_cond were already initialized by _event_reg_create
     * and must NOT be torn down here: the caller (event_loop_add)
     * unconditionally calls _event_reg_free(reg) on any non-success return
     * from this function, which is their one and only owner of destroying
     * them. Destroying them here too would be a double
     * pthread_mutex_destroy/pthread_cond_destroy, undefined behaviour per
     * POSIX. */
    return ccol_unexpected_failure;
  }
  reg->waiter_node.efd = reg->bridge_efd;

  /* See _event_loop_add_fd's identical EPOLLONESHOT-only-when-dispatch_pool
   * comment; applies here for exactly the same reason, one direction only
   * (a bridge eventfd is never shared between two regs, so there is no
   * combined-mask concern the fd case has). */
  struct epoll_event ev;
  ev.data.ptr = entry;
  ev.events = EPOLLIN | (loop->dispatch_pool ? EPOLLONESHOT : 0);
  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, reg->bridge_efd, &ev) < 0) {
    /* Only the fd itself needs closing here: it was never handed to epoll,
     * so nothing else will ever close it. wait_mtx/wait_cond are left
     * alone for _event_reg_free to tear down exactly once, same reasoning
     * as the eventfd() failure branch above. reg->waiter_node.efd is reset
     * alongside reg->bridge_efd: it was set to the same now-closed fd value
     * a few lines above, before this call; left stale it would still be
     * harmless today (the node is never linked into any queue's waiter
     * list on this failure path, so nothing ever reads it before reg is
     * freed), but resetting it keeps reg->bridge_efd and
     * reg->waiter_node.efd from ever disagreeing about whether a real,
     * open fd exists. */
    close(reg->bridge_efd);
    reg->bridge_efd = -1;
    reg->waiter_node.efd = -1;
    return ccol_unexpected_failure;
  }

  mutex_t *q_mtx;
  ccol_sel_waiter **q_head;
  ccol_sel_waiter **q_rotor; /* unused here: linking a brand-new node never
                              * needs to touch the rotor (see
                              * notify_one_sel_waiter's own doc comment) */
  _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
  (void)q_rotor;

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
  bool already_ready = _queue_sel_is_ready(&reg->sel);
  mutex_unlock(*q_mtx);

  if (already_ready) {
    _eventfd_notify(reg->bridge_efd);
  }

  return ccol_success;
}

/* Forwards a queue-backed registration's own notification onward to
 * whichever waiter is linked immediately after it in the queue's own
 * sel_{read,write}_waiters_head list, if the queue is still ready for
 * reg's own direction after this dispatch's own callback has already had
 * its chance to consume/produce. Called once, from both dispatch paths
 * (_event_loop_handle_event and _event_loop_dispatch_job_fn), immediately
 * after the callback returns, while reg is still pinned by the extra
 * refcount collection gave it (so reg itself cannot be concurrently freed
 * out from under this call).
 *
 * Necessary because, unlike a ccol_select() waiter (transient: it always
 * unlinks itself before ever re-checking readiness, so by the time it
 * forwards, the queue's CURRENT head genuinely is the next-in-line
 * waiter), an event_loop registration's waiter_node stays linked into the
 * list permanently for as long as the registration itself lives.
 * notify_one_sel_waiter() (called from every producer/consumer function in
 * this file) always wakes only the current head; without this forwarding
 * step, a second live registration on the same queue+direction (another
 * event_loop_add call, or a ccol_select() caller that started waiting
 * before this registration existed) sitting behind a permanently-linked
 * event_loop registration would never be notified again for as long as
 * that head registration stays registered: a real, silent, permanent
 * starvation bug, found by manual code review rather than any failing
 * test, since nothing elsewhere in this codebase had ever registered more
 * than one live listener on the same queue+direction at once.
 *
 * Deliberately forwards to reg->waiter_node.next (this reg's own immediate
 * successor in the list), never to the queue's current head: reg itself
 * may BE the current head (the common, single-registration case), and
 * "notify head" there would just re-notify reg itself, achieving nothing
 * useful while still paying the cost, rather than correctly reaching
 * further down the list toward whoever is actually waiting. This also
 * makes the forward strictly one hop per dispatch, never wrapping back
 * around to the head end of the list: wrapping around would let two
 * always-ready registrations (e.g. two write-direction registrations on a
 * dynamic_queue, which is almost always writable) ping-pong-notify each
 * other forever, a genuine livelock this design avoids by construction
 * rather than by accident. A backlog deeper than the number of live
 * registrations on the list may still need a fresh, unrelated send/recv to
 * fully drain (the same, already-documented "notifications aren't
 * guaranteed one-to-one with messages" characteristic a lone registration
 * already has); this function only closes the gap where a registration
 * could receive ZERO notifications forever, not that separate, pre-existing
 * coalescing characteristic.
 *
 * Reading reg->waiter_node.next is only safe under the queue's own mutex
 * (the same invariant _sel_link_waiter/_sel_unlink_waiter already rely on
 * throughout this file); reg->removed is re-checked here, under that same
 * lock, because a concurrent event_loop_remove()'s own _sel_unlink_waiter
 * call updates reg->waiter_node's NEIGHBORS' pointers but never resets
 * reg->waiter_node.next/.prev themselves once reg is spliced out, so a
 * stale post-removal read of reg->waiter_node.next could otherwise
 * dereference a node that has itself since been unlinked (and possibly
 * freed). Not needed for the removed reg's own sake: a queue owner
 * removing one listener is never responsible for waking whoever remains,
 * since every FUTURE notify_one_sel_waiter call already correctly targets
 * the list's real, current head by then; this only matters for an
 * in-flight dispatch that was already collected before a concurrent
 * removal completed.
 *
 * removed is ALSO checked once up front, lock-free, before ever touching
 * q_mtx at all: reg itself is guaranteed live here regardless (kept alive
 * by collection's own refcount bump, entirely independent of the queue's
 * lifetime), so this first check can never be unsafe, unlike locking
 * q_mtx, which requires the queue itself to still exist. Mirrors
 * _event_loop_run_callback's own identical "re-check removed at the
 * actual moment of use" precedent (see that function's own comment) and
 * closes the same class of hazard for this function's own, separate touch
 * of the queue: a caller following this module's documented "call
 * event_loop_remove, then immediately destroy the underlying queue"
 * pattern may have already done exactly that by the time this function
 * finally runs (an arbitrarily long scheduling delay on this thread,
 * exactly like the one _event_loop_run_callback's own comment already
 * documents), in which case q_mtx itself (part of the now-destroyed
 * queue) must never be locked at all. The remaining, narrower race this
 * early check alone does not fully close on its own (a concurrent
 * event_loop_remove() completing in the tiny window between this check and
 * the mutex_lock call below) is closed instead by
 * _event_loop_remove_unlink's own queue branch, which splices reg out of
 * the waiter list AND sets reg->removed in one single, uninterrupted
 * critical section on this exact same q_mtx (see that function's own
 * comment for why the two must be combined, not just individually
 * lock-protected): this function's lock/unlock below can only ever fully
 * precede or fully follow that one, never overlap it, so by the time this
 * function's own removed re-check (just below) succeeds, either reg is
 * still fully linked with removed still false (safe to read .next), or
 * reg has already been fully spliced out with removed already visible as
 * true (removed's own check short-circuits before .next is ever read). */
static void _event_loop_queue_cascade_notify_next(event_reg_s *reg) {
  if (atomic_load(&reg->removed)) return;

  mutex_t *q_mtx;
  ccol_sel_waiter **q_head;
  ccol_sel_waiter **q_rotor;
  _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
  /* Neither needed here: we forward via reg's own .next, not head, and this
   * cascade step is independent of (and does not itself advance) the
   * queue's own round-robin rotor; see notify_one_sel_waiter's doc
   * comment for why that rotor exists and why leaving it untouched here is
   * correct: it tracks "who's due for the next brand-new arrival", a
   * distinct concern from this function's own "is there a backlog to
   * surface right now" check. */
  (void)q_head;
  (void)q_rotor;

  mutex_lock(*q_mtx);
  if (!atomic_load(&reg->removed) && reg->waiter_node.next &&
      _queue_sel_is_ready(&reg->sel)) {
    _notify_waiter(reg->waiter_node.next);
  }
  mutex_unlock(*q_mtx);
}

/* Validates a ccol_selectable for event_loop_add. */
static ccol_retval_t _event_loop_validate_add_args(struct event_loop_s *loop,
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

event_reg event_loop_add(event_loop loop, ccol_selectable sel,
                         event_handlers_t handlers, void *arg, char **err_str) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw) {
    if (err_str) *err_str = CCOL_ERR_STR("Invalid or stale event_loop handle");
    return EVENT_REG_INVALID;
  }

  ccol_retval_t validate = _event_loop_validate_add_args(raw, &sel);
  if (validate != ccol_success) {
    if (err_str) *err_str = CCOL_ERR_STR("Invalid arguments to event_loop_add");
    _event_loop_resolve_unpin(raw);
    return EVENT_REG_INVALID;
  }

  event_reg_s *reg = _event_reg_create(raw, sel, handlers, arg);
  if (!reg) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate memory for event_reg");
    _event_loop_resolve_unpin(raw);
    return EVENT_REG_INVALID;
  }

  /* Slot acquisition happens BEFORE reg is wired into the fd/queue registry
   * below, not after: a real, if narrow, hazard existed when this ran last.
   * Once the target stripe's lock is released after wiring, reg is fully
   * live and dispatchable purely through the raw event_reg_s* stored in the
   * registry; the poller thread never needs reg's own public handle/slot
   * to exist at all to collect and dispatch it (for num_reactor_threads > 1,
   * this can mean handing reg->arg to a ctpool worker asynchronously). If
   * slot acquisition then failed (its only failure mode, an allocation
   * failure growing loop->reg_slots), event_loop_add reported failure to
   * the caller, who (believing no registration was ever created) had no
   * reason to think a callback might already be running, or queued, against
   * reg->arg, and could free it immediately: a genuine use-after-free,
   * requiring only a coincidence of timing (readiness racing in during the
   * brief window between wiring and slot acquisition) and an ordinary
   * allocation failure, not any additional misuse.
   *
   * Acquiring the slot first closes this completely: reg can only become
   * dispatchable once it already has a valid handle, so a slot-acquire
   * failure now means nothing was ever wired into any registry at all,
   * and there is nothing to roll back beyond releasing the just-acquired
   * slot and freeing reg directly (see the rv != ccol_success branch below).
   *
   * This ordering also has to match the only other place these two locks
   * ever nest: _cthreadcomm_atfork_prepare locks every live loop's own
   * reg_slot_mutex (via _event_reg_slot_acquire below) before any of that
   * loop's stripe locks (its own Phase 1, before stripe->lock is ever taken
   * here). Acquiring the slot before locking the stripe keeps this function
   * consistent with that existing reg_slot_mutex-then-stripe->lock order;
   * doing it the other way around would be a new stripe->lock-then-
   * reg_slot_mutex edge racing that pre-existing edge, an AB-BA deadlock
   * risk against a concurrent fork(), not merely a style choice. */
  event_reg h = _event_reg_slot_acquire(raw, reg);
  if (h == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate event_reg handle slot");
    _event_reg_free(raw, reg);
    _event_loop_resolve_unpin(raw);
    return EVENT_REG_INVALID;
  }

  /* Stripe index is computed before any lock is taken, from data already
   * available: sel.fd for fd selectables (a pure function of the fd, so a
   * second event_loop_add call for the fd's other direction independently
   * recomputes the same stripe and finds the existing entry via that
   * stripe's own chmap), or loop->next_queue_stripe's round-robin cursor
   * for queue/channel selectables (no such consistency requirement exists
   * for those; see event_loop_stripe_t's own comment). */
  size_t idx =
      (sel.type == ccol_selectable_fd)
          ? _stripe_index_for_fd(raw, sel.fd)
          : (atomic_fetch_add(&raw->next_queue_stripe, 1) % raw->num_stripes);
  event_loop_stripe_t *stripe = &raw->stripes[idx];

  mutex_lock(stripe->lock);

  ccol_retval_t rv;
  if (sel.type == ccol_selectable_fd) {
    rv = _event_loop_add_fd(raw, idx, reg);
  } else {
    event_entry *entry = _mem_calloc(raw->m_procs, 1, sizeof(event_entry));
    if (!entry) {
      rv = ccol_not_enough_memory;
    } else {
      entry->is_fd = false;
      entry->fd = -1;
      entry->stripe_idx = idx;
      entry->as.reg = reg;
      mutex_init(entry->dispatch_lock);
      atomic_init(&entry->removed, false);
      atomic_init(&entry->refcount, (size_t)0);
      /* Queue/channel selectables never share an entry (1:1, no combining),
       * so every event_loop_add call here mints a fresh generation;
       * unlike the fd path, there is no "second direction joins the
       * existing entry" case to special-case. */
      entry->generation = atomic_fetch_add(&raw->fd_generation_counter, 1) + 1;
      rv = _event_loop_add_queue(raw, entry, reg);
      if (rv == ccol_success) {
        reg->owning_entry = entry;
        reg->stripe_idx = idx;
        reg->generation = entry->generation;
        _loop_queue_list_add(stripe, reg);
      } else {
        mutex_destroy(entry->dispatch_lock);
        _mem_free(raw->m_procs, entry);
      }
    }
  }

  if (rv == ccol_success) atomic_fetch_add(&raw->reg_count, 1);

  mutex_unlock(stripe->lock);

  if (rv != ccol_success) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to register selectable with event_loop");
    /* reg already has a slot (acquired above, before wiring was ever
     * attempted), but was never actually linked into any registry: every
     * failure path inside _event_loop_add_fd/_event_loop_add_queue leaves
     * *slot/entry.as.reg back at NULL and undoes its own partial epoll_ctl
     * work before returning, so there is nothing here for
     * event_loop_remove's own two-phase unlink/finish dance to undo, and no
     * dispatch could ever have observed reg. Release the slot directly
     * (mirroring event_loop_remove's own "release the slot before reg
     * becomes eligible for reclaim" ordering, even though nothing could
     * have resolved this handle from any other thread: h was never
     * returned to any caller) and free reg outright, rather than deferring
     * it through the pending-free/epoch machinery that only exists to
     * protect an already-live registration's in-flight dispatch. */
    _event_reg_slot_release(raw, reg);
    _event_reg_free(raw, reg);
    _event_loop_resolve_unpin(raw);
    return EVENT_REG_INVALID;
  }

  _event_loop_resolve_unpin(raw);
  return h;
}

uint64_t event_loop_reg_generation(event_loop loop, event_reg reg) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw) return 0;
  event_reg_s *raw_reg = _event_reg_resolve(raw, reg);
  if (!raw_reg) {
    _event_loop_resolve_unpin(raw);
    return 0;
  }
  uint64_t gen = raw_reg->generation;
  _event_reg_resolve_unpin(raw_reg);
  _event_loop_resolve_unpin(raw);
  return gen;
}

/* Recomputes entry's current desired epoll interest mask fresh from the live
 * state of entry->as.fd.read_reg/write_reg (fd case) or entry->as.reg
 * (queue/channel case) and applies it; never a cached snapshot from an
 * earlier point in time. For the fd case, when every live direction is
 * currently paused, the computed mask carries no real interest bits at all
 * (aside from EPOLLONESHOT, when dispatch_pool exists); since the kernel
 * reports EPOLLERR/EPOLLHUP unconditionally regardless of the registered
 * mask, an empty mask cannot silence a fd that is or becomes an error/
 * hangup condition while paused; level-triggered epoll_wait would keep
 * reporting it forever, with the callback itself skipped every time by
 * reg->paused's own re-check in _event_loop_run_callback, but nothing
 * stopping the reactor from re-observing and re-collecting it on every
 * single epoll_wait call: a real, reproduced unbounded CPU-spin busy loop,
 * silently contradicting event_loop_pause's own "no callback fires,
 * exactly as if it had been removed" contract (an actually-removed
 * registration produces zero further wakeups via EPOLL_CTL_DEL; a merely
 * mask-narrowed one cannot, since ERR/HUP can't be opted out of via the
 * mask). Fixed by actually issuing EPOLL_CTL_DEL in that case, tracked via
 * entry->epoll_added: a fd already removed this way must be re-added via
 * EPOLL_CTL_ADD, not EPOLL_CTL_MOD, once some direction wants real interest
 * again, since MOD on a fd not currently in the interest set fails with
 * ENOENT; see event_entry.epoll_added's own field comment.
 * Five call sites share this: (1)
 * event_loop_modify's own direction-flip; (2) event_loop_remove's
 * partial-removal branch (one direction of an fd remains); (3)
 * event_loop_pause's own mask-exclude; (4) event_loop_resume's own
 * mask-include; (5) _event_loop_dispatch_job_fn's post-dispatch EPOLLONESHOT
 * re-arm (see _event_loop_add_fd's own comment for why every registration
 * needs EPOLLONESHOT when dispatch_pool exists). Consolidating onto one
 * implementation is what makes all five call sites race-free against each
 * other: whichever one runs later under the same stripe lock always
 * recomputes and reapplies "what should be armed right now" fresh (which
 * now includes each fd-direction reg's own paused state, not just whether a
 * slot is occupied), so e.g. a concurrent event_loop_modify and a worker's
 * re-arm for the other direction of the same fd, or an event_loop_pause
 * racing a dispatch job's own post-dispatch re-arm, can only ever be
 * redundant with each other, not racy; neither ever replays a stale mask
 * the other already moved past.
 *
 * A no-op if entry->removed is set: the entry has no live registration left
 * at all (fully removed while a dispatch job for it was still in flight),
 * and its fd may already be closed/reused by the application by now; the
 * same hazard the generation counter exists to guard callers against
 * elsewhere, applied to this module's own internal re-arm call. Also a
 * no-op for a queue entry whose entry->as.reg has already been nulled by a
 * concurrent event_loop_remove (see that function's own comment on why it
 * nulls this field before deferring the entry's free).
 *
 * Caller must already hold loop->stripes[entry->stripe_idx].lock. */
static void _event_loop_rearm_entry_locked(struct event_loop_s *loop,
                                           event_entry *entry) {
  if (atomic_load(&entry->removed)) return;

  struct epoll_event ev;
  ev.data.ptr = entry;

  if (entry->is_fd) {
    uint32_t mask = loop->dispatch_pool ? EPOLLONESHOT : 0;
    if (entry->as.fd.read_reg && !atomic_load(&entry->as.fd.read_reg->paused))
      mask |= (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
    if (entry->as.fd.write_reg && !atomic_load(&entry->as.fd.write_reg->paused))
      mask |= (EPOLLOUT | EPOLLERR | EPOLLHUP);

    /* EPOLLONESHOT alone (or a literal 0) is not real interest: every live
     * direction is currently paused. Actually drop the fd from the
     * interest set rather than handing the kernel a hollow mask that
     * still cannot suppress its unconditional EPOLLERR/EPOLLHUP
     * monitoring; see this function's own doc comment. */
    bool wants_interest = (mask & ~(uint32_t)EPOLLONESHOT) != 0;
    if (wants_interest) {
      ev.events = mask;
      int op = entry->epoll_added ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
      if (epoll_ctl(loop->epfd, op, entry->fd, &ev) == 0)
        entry->epoll_added = true;
    } else if (entry->epoll_added) {
      epoll_ctl(loop->epfd, EPOLL_CTL_DEL, entry->fd, NULL);
      entry->epoll_added = false;
    }
  } else if (entry->as.reg) {
    ev.events = EPOLLIN | (loop->dispatch_pool ? EPOLLONESHOT : 0);
    epoll_ctl(loop->epfd, EPOLL_CTL_MOD, entry->as.reg->bridge_efd, &ev);
  }
}

/* Deliberately does not also take entry->dispatch_lock: doing so would
 * require reading reg->owning_entry before the stripe lock has confirmed
 * reg->removed is false, which is exactly the unsafe read pattern the
 * owning_entry doc comment (see event_reg's own stripe_idx field) warns
 * against; entry->dispatch_lock is only ever acquired after that stripe-
 * lock-protected confirmation, in _event_loop_handle_event, never before
 * it. This function's write to reg->sel.dir below can therefore genuinely
 * run concurrently with an in-flight callback's use of *sel (a real,
 * pre-existing race, not one this module's own dispatch_lock introduces or
 * closes); see _dispatch_item.sel_snapshot's own comment for how that's
 * made safe instead: the dispatch path snapshots reg->sel under the stripe
 * lock at collection time rather than handing callbacks a live pointer
 * into reg->sel, so this function's write here (also under the stripe
 * lock) can never race a callback's read. */
ccol_retval_t event_loop_modify(event_loop loop, event_reg reg_h,
                                ccol_select_dir new_dir) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  if (new_dir != ccol_select_read && new_dir != ccol_select_write) {
    _event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  /* Resolving reg_h through loop's own reg slot table is what makes every
   * field read below safe: a stale or already-removed handle is detected
   * right here, before anything is dereferenced, rather than by touching
   * memory that might already be freed. See struct event_loop_s's own
   * reg_slots field comment. */
  event_reg_s *reg = _event_reg_resolve(raw, reg_h);
  if (!reg) {
    _event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (reg->sel.type != ccol_selectable_fd) {
    _event_reg_resolve_unpin(reg);
    _event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    mutex_unlock(stripe->lock);
    _event_reg_resolve_unpin(reg);
    _event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (reg->sel.dir == new_dir) {
    mutex_unlock(stripe->lock);
    _event_reg_resolve_unpin(reg);
    _event_loop_resolve_unpin(raw);
    return ccol_success;
  }

  /* removed was just confirmed false above, under this exact stripe's lock
   * (the only lock under which it can become true), so owning_entry is
   * guaranteed not yet freed here. */
  event_entry *entry = reg->owning_entry;
  event_reg_s **target_slot = (new_dir == ccol_select_read)
                                  ? &entry->as.fd.read_reg
                                  : &entry->as.fd.write_reg;
  if (*target_slot != NULL) {
    mutex_unlock(stripe->lock);
    _event_reg_resolve_unpin(reg);
    _event_loop_resolve_unpin(raw);
    return ccol_not_permitted;
  }

  event_reg_s **current_slot = (reg->sel.dir == ccol_select_read)
                                   ? &entry->as.fd.read_reg
                                   : &entry->as.fd.write_reg;
  *current_slot = NULL;
  *target_slot = reg;
  reg->sel.dir = new_dir;

  _event_loop_rearm_entry_locked(raw, entry);

  mutex_unlock(stripe->lock);
  _event_reg_resolve_unpin(reg);
  _event_loop_resolve_unpin(raw);
  return ccol_success;
}

/* Shared validation for event_loop_pause/event_loop_resume: both are
 * fd-only (same restriction as event_loop_modify: a queue/channel
 * registration's bridge eventfd has no equivalent "temporarily stop caring,
 * but keep the registration" use case, since nothing outside this module
 * ever touches a queue's own fd directly the way chttpserver/chttpclient
 * read/write a connection's fd themselves during a paused window), and both
 * must re-confirm reg->removed under reg's own stripe lock before touching
 * owning_entry, for the identical reason event_loop_modify already does;
 * see that function's own comment on reg->stripe_idx. Returns ccol_success
 * with *out_entry set to reg->owning_entry when the caller should proceed;
 * any other return value means the caller must unlock and return it as-is. */
static ccol_retval_t _event_loop_pause_resume_validate_locked(
    struct event_loop_s *loop, event_reg_s *reg, event_entry **out_entry) {
  (void)loop;
  if (reg->sel.type != ccol_selectable_fd) return ccol_invalid_args;
  if (atomic_load(&reg->removed)) return ccol_invalid_args;
  *out_entry = reg->owning_entry;
  return ccol_success;
}

/* @brief Temporarily stop delivering events for an fd registration, without
 * destroying it.
 *
 * Unlike event_loop_remove (which fully unregisters and defers the
 * registration for freeing), event_loop_pause leaves reg fully intact
 * (still occupying its slot on the underlying fd's entry, still counting
 * toward event_loop_reg_count, still carrying the same
 * event_loop_reg_generation) and only recomputes the fd's combined epoll
 * interest mask to exclude it. This is the cheap alternative to
 * event_loop_remove immediately followed by a later event_loop_add for a
 * caller pattern where the same logical registration is going to come back
 * (e.g. a connection handed off to a worker thread for blocking body I/O,
 * then handed back to the reactor for its next request): no heap
 * allocation/free, no fd-registry chmap churn, and one epoll_ctl call
 * instead of the two (DEL, then ADD) a remove-then-add pair costs.
 *
 * While paused, no on_readable/on_writable/on_error callback fires for reg,
 * exactly as if it had been removed; the other direction on the same fd (if
 * any) is unaffected. Pausing an already-paused reg is a no-op success.
 *
 * @param loop event_loop the registration belongs to
 * @param reg  Registration to pause
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop/reg is NULL, reg is a queue/channel
 * registration, or reg was concurrently removed
 *
 * @note Thread-safe; may be called concurrently with event_loop_remove and
 *       from within a callback running on the reactor thread
 *
 * @see event_loop_resume
 */
ccol_retval_t event_loop_pause(event_loop loop, event_reg reg_h) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;

  event_reg_s *reg = _event_reg_resolve(raw, reg_h);
  if (!reg) {
    _event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  mutex_lock(stripe->lock);

  event_entry *entry = NULL;
  ccol_retval_t rv = _event_loop_pause_resume_validate_locked(raw, reg, &entry);
  if (rv != ccol_success) {
    mutex_unlock(stripe->lock);
    _event_reg_resolve_unpin(reg);
    _event_loop_resolve_unpin(raw);
    return rv;
  }

  if (!atomic_load(&reg->paused)) {
    atomic_store(&reg->paused, true);
    _event_loop_rearm_entry_locked(raw, entry);
  }

  mutex_unlock(stripe->lock);
  _event_reg_resolve_unpin(reg);
  _event_loop_resolve_unpin(raw);
  return ccol_success;
}

/* @brief Resume event delivery for a registration previously paused by
 * event_loop_pause.
 *
 * Recomputes the fd's combined epoll interest mask to include reg again.
 * Resuming a reg that is not currently paused (never paused, or already
 * resumed) is a no-op success. This deliberately mirrors
 * event_loop_modify's own "already in the requested state" idempotence
 * rather than treating it as an error, since a caller racing its own
 * pause/resume pairing against a concurrent event_loop_remove should not
 * need to distinguish "already resumed" from "nothing to do" by return
 * value alone.
 *
 * @param loop event_loop the registration belongs to
 * @param reg  Registration to resume
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if loop/reg is NULL, reg is a queue/channel
 * registration, or reg was concurrently removed (e.g. the connection was
 * closed while the caller still thought it owned a paused registration to
 * resume)
 *
 * @note Thread-safe; may be called concurrently with event_loop_remove and
 *       from within a callback running on the reactor thread
 *
 * @see event_loop_pause
 */
ccol_retval_t event_loop_resume(event_loop loop, event_reg reg_h) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;

  event_reg_s *reg = _event_reg_resolve(raw, reg_h);
  if (!reg) {
    _event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  mutex_lock(stripe->lock);

  event_entry *entry = NULL;
  ccol_retval_t rv = _event_loop_pause_resume_validate_locked(raw, reg, &entry);
  if (rv != ccol_success) {
    mutex_unlock(stripe->lock);
    _event_reg_resolve_unpin(reg);
    _event_loop_resolve_unpin(raw);
    return rv;
  }

  if (atomic_load(&reg->paused)) {
    atomic_store(&reg->paused, false);
    _event_loop_rearm_entry_locked(raw, entry);
  }

  mutex_unlock(stripe->lock);
  _event_reg_resolve_unpin(reg);
  _event_loop_resolve_unpin(raw);
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
 * own next batch boundary" no longer means anything; ANY of the N threads
 * could be mid-processing a stale, already-fetched batch that references
 * this entry, independent of which thread happens to reach its own next
 * batch boundary first. See the large comment above
 * _event_loop_reclaim_pending_frees for the epoch-based scheme this module
 * uses to generalize the same guarantee to N threads; this function only
 * pushes the node with its defer_gen snapshot stamped, it does not decide
 * when freeing is actually safe.
 *
 * Lock-free Treiber-stack push (loop->pending_entry_frees is loop-wide
 * aggregate state, not per-stripe; entries from every stripe are threaded
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
  atomic_store(&entry->removed, true);
  entry->defer_gen = atomic_load(&loop->poller_batch_gen);
  _event_loop_push_entry_free_node(loop, entry);
}

/* event_reg cannot be freed synchronously either, for a related but
 * distinct reason from _event_loop_defer_entry_free's: an in-flight
 * dispatch callback (refcount > 1 at the moment of removal) may still need
 * reg after event_loop_remove returns. Deferring the free until refcount
 * genuinely reaches 0 (checked by the caller before this is ever invoked)
 * keeps the memory valid for that callback to finish safely; once
 * deferred, actual reclamation additionally waits for
 * reg->pending_resolve_count to reach 0 (see struct event_loop_s's own
 * reg_slots field comment and _event_loop_reclaim_pending_frees), which is
 * what makes it safe for event_loop_remove and event_loop_modify to be
 * documented as gracefully returning ccol_invalid_args, rather than
 * invoking undefined behaviour, when raced against each other on the same
 * reg. Same lock-free Treiber-stack shape as
 * _event_loop_push_entry_free_node/_event_loop_defer_entry_free. */
static void _event_loop_push_reg_free_node(struct event_loop_s *loop,
                                           event_reg_s *reg) {
  event_reg_s *old_head = atomic_load(&loop->pending_reg_frees);
  do {
    reg->pending_free_next = old_head;
  } while (
      !atomic_compare_exchange_weak(&loop->pending_reg_frees, &old_head, reg));
}

static void _event_loop_defer_reg_free(struct event_loop_s *loop,
                                       event_reg_s *reg) {
  _event_loop_push_reg_free_node(loop, reg);
}

/* Reclaims every entry/reg deferred by _event_loop_defer_entry_free /
 * _event_loop_defer_reg_free that is now provably safe to free, and
 * otherwise leaves it pending for a later attempt.
 *
 * Exactly one thread (poller_thread) ever calls epoll_wait, for every
 * configuration, so "safe to free" reduces to a single scalar comparison:
 * this function is called by poller_thread at its own between-batches
 * point (see _event_loop_thread_fn), after it has already published its
 * own advanced poller_batch_gen. A reg is eligible once its defer_gen
 * snapshot is older than the current poller_batch_gen: that means
 * poller_thread has crossed a between-batches point at least once since the
 * item was deferred, so it can no longer be mid-processing whatever batch
 * (if any) it had in flight at defer time; and since the item was already
 * removed from the registry (epoll_ctl(DEL) or slot-clearing) before it was
 * ever deferred, no FUTURE batch can reference it either.
 *
 * An entry additionally requires refcount == 0 before the epoch condition
 * alone is sufficient (see event_entry.refcount's own comment): with
 * num_reactor_threads > 1, a ctpool worker's dispatch job can still be
 * using an entry well after poller_thread has moved past the epoch it was
 * deferred at, and the epoch check on its own says nothing about that. reg
 * needs no such additional check here; its own pre-existing refcount
 * mechanism already prevents it from ever being deferred in the first
 * place while a callback (inline or on a worker) still needs it; see
 * _event_loop_release_after_dispatch.
 *
 * Lock-free: each list's entire contents are claimed in one
 * atomic_exchange, partitioned outside of any lock into "free now" and
 * "not yet eligible", and anything not yet eligible is individually pushed
 * back via the same CAS-based push the original defer call used; WITHOUT
 * re-stamping defer_gen, since these items' original snapshot is what lets
 * them make forward progress; re-stamping would reset their eligibility
 * clock every time a reclaim attempt finds them still-pending and could
 * starve them indefinitely under sustained load. Also used, in its
 * unconditional form (see _event_loop_free_all_pending), by
 * __event_loop_destroy after poller_thread (and dispatch_pool, if any) has
 * been fully joined/drained, where no epoch or refcount check is needed at
 * all since no thread can reference anything any more. */
static void _event_loop_reclaim_pending_frees(struct event_loop_s *loop) {
  size_t reached = atomic_load(&loop->poller_batch_gen);

  event_entry *e = atomic_exchange(&loop->pending_entry_frees, NULL);
  event_entry *e_keep = NULL;
  while (e) {
    event_entry *next = e->pending_free_next;
    if (e->defer_gen < reached && atomic_load(&e->refcount) == 0) {
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

  /* reg's own eligibility condition is pending_resolve_count == 0, not an
   * epoch comparison: see struct event_loop_s's own reg_slots field
   * comment. Its slot (if it had one) was already released, under
   * loop->reg_slot_mutex, at the exact point it was deferred (see
   * event_loop_remove's own two-phase call site), so no NEW resolve of it
   * can ever succeed from that point onward; pending_resolve_count == 0
   * therefore means no thread, anywhere, still holds a pointer to this reg
   * from an in-flight event_loop_modify/_pause/_resume/_remove/
   * event_loop_reg_generation call, making it safe to free outright with
   * no further lock needed. */
  event_reg_s *r = atomic_exchange(&loop->pending_reg_frees, NULL);
  event_reg_s *r_keep = NULL;
  while (r) {
    event_reg_s *next = r->pending_free_next;
    if (atomic_load(&r->pending_resolve_count) == 0) {
      _event_reg_free(loop, r);
    } else {
      r->pending_free_next = r_keep;
      r_keep = r;
    }
    r = next;
  }
  while (r_keep) {
    event_reg_s *next = r_keep->pending_free_next;
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
  event_reg_s *r = atomic_exchange(&loop->pending_reg_frees, NULL);
  while (r) {
    event_reg_s *next = r->pending_free_next;
    _event_reg_free(loop, r);
    r = next;
  }
}

/* Phase 1 of the shared core of event_loop_remove, taking an
 * already-resolved raw reg pointer directly rather than a handle: used by
 * the public event_loop_remove, after resolving+pinning reg via
 * _event_reg_resolve. event_loop_add has no use for this: it acquires
 * reg's own slot BEFORE ever wiring reg into the fd/queue registry (see
 * its own comment on _event_reg_slot_acquire), so a wiring failure there
 * always finds reg still fully unlinked, with nothing for this function's
 * own unlink logic to do; event_loop_add releases the slot and frees reg
 * directly in that case instead.
 * Unlinks reg from the fd/queue registry and marks it removed, but does
 * NOT touch refcount or defer it for reclaim yet; see
 * _event_loop_remove_finish, phase 2, and event_loop_remove's own call
 * site for why that split matters: reg's own slot (when it has one) must
 * be released BETWEEN these two phases, before reg is ever pushed onto
 * loop->pending_reg_frees, or a second resolve racing the slot release
 * could still succeed against a reg already eligible for actual reclaim.
 * Returns true if this call is the one that actually performed the
 * removal (reg->removed was false and is now true), false if reg was
 * already removed by a prior call (a graceful no-op; the caller must not
 * proceed to phase 2 or release a slot in that case). */
static bool _event_loop_remove_unlink(struct event_loop_s *raw,
                                      event_reg_s *reg) {
  event_loop_stripe_t *stripe = &raw->stripes[reg->stripe_idx];
  mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    mutex_unlock(stripe->lock);
    return false;
  }

  event_entry *entry = reg->owning_entry;

  if (reg->sel.type == ccol_selectable_fd) {
    event_reg_s **slot = (reg->sel.dir == ccol_select_read)
                             ? &entry->as.fd.read_reg
                             : &entry->as.fd.write_reg;
    *slot = NULL;
    if (entry->as.fd.read_reg == NULL && entry->as.fd.write_reg == NULL) {
      /* entry->epoll_added may already be false here (every direction was
       * paused, so _event_loop_rearm_entry_locked already issued its own
       * EPOLL_CTL_DEL; see that function's own comment); only issue a
       * second one when the fd is actually still registered. */
      if (entry->epoll_added)
        epoll_ctl(raw->epfd, EPOLL_CTL_DEL, entry->fd, NULL);
      _fd_registry_remove(stripe, entry->fd);
      _event_loop_defer_entry_free(raw, entry);
    } else {
      _event_loop_rearm_entry_locked(raw, entry);
    }
  } else {
    mutex_t *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);

    /* The splice (unlinking reg from the queue's own waiter list) and the
     * removed=true store MUST happen under ONE SINGLE q_mtx critical
     * section, not two separate lock/unlock pairs on the same mutex: once
     * the splice runs, reg->waiter_node.next/.prev are frozen (splicing
     * only updates reg's NEIGHBORS' pointers, never reg's own, since reg
     * is no longer linked for any future splice to touch) - see
     * _sel_unlink_waiter_locked. If removed were set to true only in a
     * later, separate critical section, a concurrent
     * _event_loop_queue_cascade_notify_next(reg) call (running on another
     * thread, still mid-dispatch of reg's own already-collected callback;
     * event_loop_remove does not wait for that to finish) could lock q_mtx
     * in the gap between the two, observe removed still false, and read
     * reg->waiter_node.next - which is already stale at that point,
     * frozen at whichever registration was reg's successor at splice
     * time. If a THIRD, independent event_loop_remove() call on that
     * successor completes and reclaims it before cascade-notify gets
     * there (event_reg reclaim has no epoch delay once its refcount and
     * pending_resolve_count both reach 0), cascade-notify would call
     * _notify_waiter() on already-freed memory. Combining the two
     * mutations into one critical section closes this: by the time
     * cascade-notify's own lock(q_mtx) succeeds, either both the splice
     * and removed=true have already happened together (so removed reads
     * true and .next is never touched) or neither has (so the queue,
     * and every registration still linked to it, is still fully intact).
     *
     * This is what makes it safe for a caller to follow this module's own
     * documented "call event_loop_remove, then immediately free/destroy
     * the underlying resource" pattern (already relied on for fd
     * selectables via _event_loop_run_callback's own removed re-check)
     * for a QUEUE selectable too, now that
     * _event_loop_queue_cascade_notify_next also touches this same queue
     * after dispatch: that function's own
     * lock(q_mtx)-check(removed)-maybe-notify-unlock(q_mtx) critical
     * section can only ever fully precede or fully follow this one, never
     * overlap it (mutexes guarantee that). If it precedes, the queue (and
     * every registration reg's own .next could point to) is obviously
     * still alive. If it follows, removed is already visible as true the
     * moment that function's own lock succeeds (mutex release/acquire is
     * itself a memory barrier), so it touches reg->waiter_node.next at
     * all. Found via valgrind while adding the cascade fix, not by
     * inspection: reverting just this fix (splitting the splice and the
     * removed=true store back into two separate critical sections)
     * reliably reproduces a heap-use-after-free (mutex_lock on an
     * already-destroyed queue's own mutex) under a "wait for the
     * callback's own signal, then remove and immediately destroy the
     * queue from a different thread" test pattern. */
    mutex_lock(*q_mtx);
    _sel_unlink_waiter_locked(&reg->waiter_node, q_head, q_rotor);
    atomic_store(&reg->removed, true);
    mutex_unlock(*q_mtx);

    epoll_ctl(raw->epfd, EPOLL_CTL_DEL, reg->bridge_efd, NULL);
    _loop_queue_list_remove(stripe, reg);
    /* entry itself is deferred (safe, still-valid memory) below, but its
     * as.reg field must be nulled HERE, under stripe->lock, before that; a
     * stale batch entry could otherwise read entry->as.reg after reg is
     * deferred-freed and get a still-dangling-looking pointer into the
     * pending-free list rather than a clean NULL. The fd branch above
     * already does the equivalent via *slot = NULL. */
    entry->as.reg = NULL;
    _event_loop_defer_entry_free(raw, entry);
  }

  if (reg->sel.type == ccol_selectable_fd) {
    atomic_store(&reg->removed, true);
  }

  mutex_unlock(stripe->lock);
  return true;
}

/* Phase 2: decrements refcount and, if this was the last reference, defers
 * reg for reclaim. Must run strictly AFTER reg's own slot (if it has one)
 * has already been released, so reg is never simultaneously "on the
 * pending-free list" and "still resolvable via its old handle"; see
 * _event_loop_remove_unlink's own comment for why. */
static void _event_loop_remove_finish(struct event_loop_s *raw,
                                      event_reg_s *reg) {
  atomic_fetch_sub(&raw->reg_count, 1);

  int prev = atomic_fetch_sub(&reg->refcount, 1);
  /* Deferred, not freed here directly: an in-flight callback (if any) may
   * still need reg; see _event_loop_defer_reg_free's own comment and
   * _event_loop_reclaim_pending_frees's for the full reclamation scheme,
   * gated on reg->pending_resolve_count reaching 0 (see struct
   * event_loop_s's own reg_slots field comment). */
  if (prev == 1) _event_loop_defer_reg_free(raw, reg);
}

ccol_retval_t event_loop_remove(event_loop loop, event_reg reg_h) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  event_reg_s *reg = _event_reg_resolve(raw, reg_h);
  if (!reg) {
    _event_loop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (_event_loop_remove_unlink(raw, reg)) {
    /* Immediately invalidates this reg's own handle for every future
     * resolve attempt, BEFORE phase 2 below can ever make reg eligible
     * for actual reclaim; see _event_loop_remove_unlink's own comment for
     * why this ordering, not "release the slot after phase 2", is the one
     * that is actually race-free. Skipped when this call observed reg
     * already removed by an earlier one, which already did this. */
    _event_reg_slot_release(raw, reg);
    _event_loop_remove_finish(raw, reg);
  }

  /* Safe even though reg may have just been deferred for reclaim: this
   * call's own still-held pin (from the successful resolve above) is
   * exactly what guarantees _event_loop_reclaim_pending_frees cannot have
   * freed it yet. */
  _event_reg_resolve_unpin(reg);
  _event_loop_resolve_unpin(raw);
  return ccol_success;
}

size_t event_loop_reg_count(event_loop loop) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw) return ccol_invalid_size;
  size_t n = atomic_load(&raw->reg_count);
  _event_loop_resolve_unpin(raw);
  return n;
}

/* One reg collected for dispatch under the entry's stripe lock, acted on
 * after that lock is released. */
typedef struct _dispatch_item {
  event_reg_s *reg;
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
   * `&reg->sel` pointer *after* releasing the stripe lock; a genuine,
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
   * satisfied; collection re-reads reg->sel fresh every single dispatch,
   * so an event_loop_modify call that completed before this collection is
   * still correctly observed; only a modify running fully concurrently
   * with an in-flight callback (previously a data race with an undefined
   * outcome) now deterministically resolves to whichever direction was
   * current at collection time. */
  ccol_selectable sel_snapshot;
} _dispatch_item;

static void _event_loop_run_callback(event_loop loop, _dispatch_item *item) {
  event_reg_s *reg = item->reg;
  /* Re-check removed here, at the actual moment of invocation, not just at
   * collection time (where this was already checked once, under the stripe
   * lock, before this item was ever added to items[]/job->items[]).
   * event_loop_remove() does not take entry->dispatch_lock and does not wait
   * for an already-collected item to finish dispatching (see its own header
   * doc comment: "in-progress" callback teardown is deferred, which protects
   * event_loop's own reg/entry memory, but promises nothing about whether a
   * collected-but-not-yet-invoked callback still fires); so a concurrent
   * event_loop_remove can complete, and the caller can go on to free
   * whatever reg->arg points to, strictly between collection and this
   * function actually running. For num_reactor_threads == 1 that window is
   * a handful of instructions with no thread switch possible in between
   * (collection and this call happen back-to-back in the same function,
   * same thread); real, but so narrow it was never observed. For
   * num_reactor_threads > 1 the equivalent window is an arbitrarily long
   * ctpool queue wait, which made this a real, valgrind-caught
   * use-after-free (found via tests/chttpserver/tests_mem_mgmt.c's own
   * teardown racing a freshly accepted connection's first readable
   * dispatch against __chttpsvr_destroy's _close_all_idle_connections):
   * chttpserver's own conn->reg lifetime discipline is "call
   * event_loop_remove, then immediately free conn" (see chttpserver.c's
   * _conn_close), which is only safe if event_loop guarantees no callback
   * still runs afterward with conn as reg->arg. Re-checking removed here
   * closes that gap for both dispatch paths with one change: once removed
   * is observed true, reg->arg is never touched again by this reg. */
  if (atomic_load(&reg->removed)) return;
  /* Re-check paused here for the identical reason removed is re-checked
   * above: event_loop_pause's own contract ("no on_readable/on_writable/
   * on_error callback fires for reg" while paused) is enforced by
   * recomputing the fd's epoll interest mask, which only prevents a FUTURE
   * epoll_wait from reporting readiness for this reg; it does nothing about
   * an event already collected into a job before the pause() call took
   * effect. For num_reactor_threads == 1 that window is negligible
   * (collection and this call happen back-to-back with no lock release in
   * between); for num_reactor_threads > 1 it is an arbitrarily long ctpool
   * queue wait, wide enough to be observed in practice under valgrind
   * (event_loop.pause_write_direction: a pipe's write end is writable from
   * the instant it exists, so the poller can collect-and-submit a dispatch
   * job for it before the test's own very next line ever calls
   * event_loop_pause). Skipping here, rather than only at collection time,
   * closes that window the same way the removed check above does. */
  if (atomic_load(&reg->paused)) return;
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
 * all; not even a stripe lock, since nothing here touches entry state. */
static void _event_loop_release_after_dispatch(struct event_loop_s *loop,
                                               event_reg_s *reg) {
  int prev = atomic_fetch_sub(&reg->refcount, 1);
  if (prev == 1 && atomic_load(&reg->removed)) {
    _event_loop_defer_reg_free(loop, reg);
  }
}

/* The num_reactor_threads == 1 dispatch path ONLY; kept byte-for-byte
 * unchanged from before this module's poller/dispatch_pool split (see
 * struct event_loop_s's own field comments and _event_loop_thread_fn's
 * branch on loop->dispatch_pool). For num_reactor_threads > 1, this
 * function is never called at all; see _event_loop_poller_collect (the
 * collection half) and _event_loop_dispatch_job_fn (the dispatch half,
 * running on a ctpool worker) instead. Called once per epoll_event
 * returned by epoll_wait, on poller_thread (the only thread that ever
 * calls epoll_wait, for both configurations). Collects live regs to
 * dispatch under entry->stripe_idx's own stripe lock (incrementing each
 * one's refcount so it can't be freed while its callback runs), then
 * releases that lock and runs callbacks unlocked; never holding a stripe
 * lock across a user callback.
 *
 * Held across the WHOLE function, not just collection: entry->dispatch_lock.
 * For num_reactor_threads == 1 specifically this lock is uncontended by
 * construction (poller_thread is the only caller of this function, so
 * there is no other thread that could race it here); it is kept anyway
 * purely so this function's own logic needs no special-casing versus its
 * pre-split form, matching the "byte-for-byte unchanged" requirement above
 * exactly. Its original motivating hazard (more than one thread each
 * independently calling epoll_wait on the same shared epoll instance,
 * genuinely receiving the same still-ready entry more than once, and
 * without this lock both passing the collection step below for the SAME
 * reg and invoking its callback concurrently; a real double-dispatch, not
 * a memory-safety bug, but a serialization bug this lock existed
 * specifically to close) no longer applies to this exact function post-
 * split, since only ever one thread calls it now regardless of
 * num_reactor_threads; that hazard's replacement for num_reactor_threads >
 * 1 is instead closed by _event_loop_dispatch_job_fn's own use of this
 * same lock, moved to the ctpool worker that actually runs a job; see
 * that function's own comment. Because dispatch_lock lives on the entry
 * (shared by both directions), it also gives read_reg and write_reg
 * callbacks on the same fd mutual exclusion against each other, not just
 * self-exclusion, in both configurations; see the entry's own
 * dispatch_lock field comment for why that's a deliberately strict
 * guarantee. */
static void _event_loop_handle_event(struct event_loop_s *loop,
                                     struct epoll_event *ev) {
  event_entry *entry = (event_entry *)ev->data.ptr;

  if (entry == NULL) {
    /* The shutdown-eventfd's ev.data.ptr is left NULL; it exists purely to
     * interrupt epoll_wait, nothing to dispatch. Deliberately NOT drained
     * via read() here; with more than one reactor thread, an earlier
     * version of this function did drain it, and that was a real,
     * reproduced deadlock: whichever thread happened to process this event
     * FIRST reset the eventfd's counter to 0, and any other thread still
     * blocked in its own epoll_wait call at that moment (with timeout=-1)
     * then had nothing left to observe as ready and never returned,
     * leaving event_loop_shutdown's join loop hung on it forever. Leaving
     * the counter non-zero and never draining it means every thread's
     * epoll_wait call (whichever order they happen to run in, already
     * blocked or not yet called) keeps seeing this fd as ready
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
   * (inserted into a stripe's chmap / epoll_ctl'd), and never again; safe
   * to read here with no lock held yet. */
  event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];
  mutex_lock(stripe->lock);

  if (entry->is_fd) {
    bool is_err = (ev->events & (EPOLLERR | EPOLLHUP)) != 0;
    bool is_in = (ev->events & (EPOLLIN | EPOLLRDHUP)) != 0;
    bool is_out = (ev->events & EPOLLOUT) != 0;

    event_reg_s *r = entry->as.fd.read_reg;
    if (r && !atomic_load(&r->removed) && (is_err || is_in)) {
      atomic_fetch_add(&r->refcount, 1);
      items[n_items].reg = r;
      /* EPOLLIN/EPOLLRDHUP and EPOLLERR/EPOLLHUP are not mutually
       * exclusive: the kernel legitimately reports both together on the
       * SAME event when a peer writes data and then immediately closes
       * (or resets) the connection; the bytes are genuinely sitting in
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
       * error-only registration (on_readable == NULL, on_error set;
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
    event_reg_s *w = entry->as.fd.write_reg;
    if (w && !atomic_load(&w->removed) && (is_err || is_out)) {
      atomic_fetch_add(&w->refcount, 1);
      items[n_items].reg = w;
      /* Mirrors the read-direction has_reader fix immediately above, for
       * the identical reason: EPOLLOUT and EPOLLERR/EPOLLHUP are not
       * mutually exclusive either. A socket that has just entered an error
       * state (peer RST, etc.) is reported as writable too, since write(2)
       * on it would return immediately with an error rather than block;
       * unconditionally prioritising is_err here silently starved a
       * write-only registration (on_writable set, on_error == NULL;
       * explicitly documented as a supported pattern on event_handlers_t
       * itself: "a write-only producer that never expects on_error may
       * pass NULL there") of every future on_writable dispatch, forever,
       * the instant such an error condition first co-occurred with
       * writability: is_error was set unconditionally true with no
       * on_error handler to consume it, so _event_loop_run_callback's
       * on_error branch silently did nothing, on_writable was never even
       * attempted, and the identical (level-triggered) event kept
       * re-dispatching to the same dead end on every subsequent
       * epoll_wait; the same "silent, unbounded dispatch spin" failure
       * class has_reader already exists to prevent on the read side. */
      bool has_writer = (w->handlers.on_writable != NULL);
      items[n_items].is_writable = is_out && has_writer;
      items[n_items].is_error = is_err && !items[n_items].is_writable;
      items[n_items].is_readable = false;
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
    event_reg_s *r = entry->as.reg;
    if (r) {
      _eventfd_drain(r->bridge_efd);
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
    _event_loop_run_callback(loop->self_handle, &items[i]);
    /* Queue-backed only (see _event_loop_queue_cascade_notify_next's own
     * comment); fd entries have no waiter list to forward through. Called
     * while items[i].reg is still pinned by collection's own refcount bump,
     * strictly before that pin is released below. */
    if (!entry->is_fd) _event_loop_queue_cascade_notify_next(items[i].reg);
    _event_loop_release_after_dispatch(loop, items[i].reg);
  }

  mutex_unlock(entry->dispatch_lock);
}

/* One collected dispatch batch (up to 2 items, exactly like
 * _event_loop_handle_event's own stack-local items[2]) for a single entry,
 * submitted as one ctpool job so both items (if there are two) still
 * run back-to-back under one entry->dispatch_lock acquisition, on one
 * worker, preserving _event_loop_handle_event's existing atomicity/ordering
 * for the num_reactor_threads == 1 path exactly, and matching it for the
 * > 1 path rather than introducing a new, never-before-reasoned-about
 * interleaving. Heap-allocated (unlike _event_loop_handle_event's
 * stack-local array) because it must outlive the poller's own collection
 * call to reach whichever ctpool worker eventually runs it. */
typedef struct _dispatch_job {
  struct event_loop_s *loop;
  event_entry *entry;
  _dispatch_item items[2];
  size_t n_items;
} _dispatch_job;

/* Forward declaration: _event_loop_poller_collect (below) submits jobs to
 * this function via ctpool_submit before its own definition appears later
 * in this file. */
static void _event_loop_dispatch_job_fn(void *arg);

/* Process-wide, lazily-created thread-local key: holds the event_loop
 * (opaque `void *`) whose dispatch job is currently executing on the
 * calling thread, or NULL. Used solely by event_loop_shutdown's
 * self-call guard (see its own comment). Deliberately process-wide rather
 * than per-loop: a ctpool worker thread is always privately owned by
 * exactly one event_loop instance's own dispatch_pool for its entire
 * lifetime (ctpool always spawns and owns its own dedicated OS threads,
 * never shared across pools), so one process-wide key correctly
 * disambiguates "which loop's job (if any) is running on me right now" for
 * every event_loop instance in the process, including the
 * multiple-independent-instances case, with no per-loop key lifecycle to
 * manage. Never torn down (thread_ls_key_delete'd) at any single loop's
 * destruction; matches this codebase's own established pattern for
 * process-wide lazily-created statics (e.g. chttpclient.c's
 * g_chttp1_settings_once) that live for the process's lifetime. */
static struct {
  thread_ls_key_t key;
  once_flag_t once;
} event_loop_job_key_bundle = {0};

static void _event_loop_init_job_key(void) {
  thread_ls_key_create(event_loop_job_key_bundle.key, NULL);
}

/* N>1 poller path: collects dispatch items for entry under the entry's
 * stripe lock only (no entry->dispatch_lock; see the design comment
 * above _event_loop_handle_event for why the num_reactor_threads == 1 path
 * needs it there and this path structurally cannot race itself the same
 * way: there is exactly one poller_thread, so collection is inherently
 * serial regardless; entry->dispatch_lock is acquired only later, by
 * whichever ctpool worker actually runs the job, exactly mirroring how
 * event_loop_modify already never takes it either; see that function's
 * own comment), bumps entry's refcount (protects the job about to be
 * submitted from a concurrent event_loop_remove/reclaim; see
 * event_entry.refcount's own comment), and submits a heap-allocated job to
 * dispatch_pool for a worker to actually run.
 *
 * Deliberately near-identical to (not sharing code with)
 * _event_loop_handle_event's own collection logic: keeping
 * num_reactor_threads == 1 byte-for-byte unchanged (a hard requirement;
 * see struct event_loop_s's own field comment) means this file now carries
 * two distinct dispatch code paths rather than one unified implementation.
 * This is a deliberate complexity-for-correctness/performance trade, not a
 * free simplification; real ongoing maintenance surface, not glossed over.
 *
 * Unlike _event_loop_handle_event, a queue selectable's bridge eventfd is
 * NOT drained here; see _event_loop_dispatch_job_fn's own comment for why
 * that must wait until the job actually runs on a worker. */
static void _event_loop_poller_collect(struct event_loop_s *loop,
                                       struct epoll_event *ev) {
  event_entry *entry = (event_entry *)ev->data.ptr;
  if (entry == NULL)
    return; /* shutdown sentinel; see _event_loop_handle_event's identical
               comment */

  event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];
  mutex_lock(stripe->lock);

  /* A job is already queued or executing for this entry; drop this event
   * entirely rather than submit a second, concurrent one. Found via a real,
   * valgrind-caught use-after-free (not by inspection): application code is
   * explicitly permitted, and does, call event_loop_modify from WITHIN an
   * in-flight callback (e.g. chttpclient.c's own TLS/request state machine
   * flipping WANT_READ/WANT_WRITE as it advances); and event_loop_modify
   * re-arms EPOLLONESHOT via this same shared helper (see
   * _event_loop_rearm_entry_locked), same as a genuine post-dispatch
   * re-arm. If the fd is already ready again at that moment (a real
   * response can arrive within microseconds on loopback), the poller can
   * collect and submit a SECOND job for this entry while the FIRST job's
   * callback is still executing (and, in the reproduced crash, about to
   * free application state the second job's callback would then read after
   * the first job's dispatch_lock-protected callback (correctly
   * serialized to run strictly AFTER the first) released dispatch_lock
   * only once already-freed). entry->refcount (bumped below, released only
   * once the in-flight job's own dispatch_lock-protected callback AND its
   * own post-dispatch re-arm have both completed; see
   * _event_loop_dispatch_job_fn) is exactly "is there already a job for
   * this entry that hasn't reached that point yet", checked under the same
   * stripe lock that guards the corresponding decrement, so there is no
   * window to observe it inconsistently. Dropping this event is safe:
   * the in-flight job's own eventual re-arm (not this one) is what
   * correctly reflects state as of when it actually finishes, and a
   * genuinely new readiness after that point will be reported fresh. */
  if (atomic_load(&entry->refcount) > 0) {
    mutex_unlock(stripe->lock);
    return;
  }

  _dispatch_job *job = _mem_alloc(loop->m_procs, sizeof(_dispatch_job));
  if (!job) {
    /* Extremely unlikely (small, fixed-size allocation). Nothing was
     * bumped yet, so there is nothing to unwind; drop this dispatch, but
     * still re-arm; unlike plain level-triggered epoll (where a dropped
     * event would naturally reappear on a future epoll_wait for free), the
     * kernel disarms an EPOLLONESHOT registration the moment it reports an
     * event, regardless of what userspace does with it; skipping this
     * re-arm would leave the entry permanently un-reported from here on,
     * not merely delayed (a real, if OOM-only-triggered, gap found while
     * fixing the use-after-free above, not by inspection: an earlier draft
     * of this comment claimed level-triggered semantics would cover this,
     * which is only true without EPOLLONESHOT). */
    _event_loop_rearm_entry_locked(loop, entry);
    mutex_unlock(stripe->lock);
    return;
  }
  job->loop = loop;
  job->entry = entry;
  job->n_items = 0;

  if (entry->is_fd) {
    bool is_err = (ev->events & (EPOLLERR | EPOLLHUP)) != 0;
    bool is_in = (ev->events & (EPOLLIN | EPOLLRDHUP)) != 0;
    bool is_out = (ev->events & EPOLLOUT) != 0;

    /* Mirrors _event_loop_handle_event's own EPOLLIN-vs-EPOLLERR/EPOLLHUP
     * dispatch-priority logic exactly; see that function's own comment for
     * the full reasoning (readable wins only when there is an actual
     * reader registered to hand the data to). */
    event_reg_s *r = entry->as.fd.read_reg;
    if (r && !atomic_load(&r->removed) && (is_err || is_in)) {
      atomic_fetch_add(&r->refcount, 1);
      _dispatch_item *item = &job->items[job->n_items];
      item->reg = r;
      bool has_reader = (r->handlers.on_readable != NULL);
      item->is_readable = is_in && has_reader;
      item->is_error = is_err && !item->is_readable;
      item->is_writable = false;
      item->sel_snapshot = r->sel;
      job->n_items++;
    }
    /* Mirrors the read-direction has_reader guard above, and
     * _event_loop_handle_event's own identical has_writer fix, for the
     * identical reason: EPOLLOUT and EPOLLERR/EPOLLHUP are not mutually
     * exclusive either (a socket that just entered an error state is
     * reported writable too, since write(2) on it returns immediately
     * rather than blocking). Without has_writer, a write-only registration
     * (on_writable set, on_error == NULL, an explicitly documented
     * supported pattern) would be silently, permanently starved of
     * on_writable the instant such an error co-occurred with writability. */
    event_reg_s *w = entry->as.fd.write_reg;
    if (w && !atomic_load(&w->removed) && (is_err || is_out)) {
      atomic_fetch_add(&w->refcount, 1);
      _dispatch_item *item = &job->items[job->n_items];
      item->reg = w;
      bool has_writer = (w->handlers.on_writable != NULL);
      item->is_writable = is_out && has_writer;
      item->is_error = is_err && !item->is_writable;
      item->is_readable = false;
      item->sel_snapshot = w->sel;
      job->n_items++;
    }
  } else {
    /* Queue entry: entry->as.reg can legitimately be NULL here (a
     * concurrent event_loop_remove nulls it under this exact stripe lock
     * before deferring entry's own free); see
     * _event_loop_handle_event's identical check for why. Bridge eventfd
     * drain deliberately deferred to _event_loop_dispatch_job_fn. */
    event_reg_s *r = entry->as.reg;
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
    /* Nothing live to dispatch (e.g. every candidate reg was already
     * removed by the time collection ran); no entry refcount was bumped,
     * nothing to submit. Note this entry is left un-rearmed if
     * EPOLLONESHOT already fired for it; that is fine, since with no live
     * reg left there is nothing that should ever be notified again for it
     * anyway (event_loop_remove's own EPOLL_CTL_DEL/MOD calls are what
     * actually own this entry's real interest state going forward). */
    _mem_free(loop->m_procs, job);
    mutex_unlock(stripe->lock);
    return;
  }

  atomic_fetch_add(&entry->refcount, 1);
  mutex_unlock(stripe->lock);

  ccol_retval_t rv = ctpool_submit(loop->dispatch_pool,
                                   _event_loop_dispatch_job_fn, job, NULL);
  if (rv != ccol_success) {
    /* Extremely unlikely (ccol_not_enough_memory is ctpool_submit's only
     * real failure mode on this module's own always-unbounded queue).
     * Unwind exactly what was bumped above and drop the dispatch. Re-arm
     * (re-acquiring the stripe lock, released above before the submit
     * call) for the same reason the job-alloc-failure branch above does:
     * EPOLLONESHOT is already consumed at this point regardless of
     * submission success, so skipping this would leave the entry
     * permanently un-reported, not merely delayed. Refcount release and
     * re-arm are done together under the lock, same as
     * _event_loop_dispatch_job_fn's own pairing and for the identical
     * reason (see that function's comment). */
    for (size_t i = 0; i < job->n_items; i++)
      _event_loop_release_after_dispatch(loop, job->items[i].reg);
    mutex_lock(stripe->lock);
    _event_loop_rearm_entry_locked(loop, entry);
    atomic_fetch_sub(&entry->refcount, 1);
    mutex_unlock(stripe->lock);
    _mem_free(loop->m_procs, job);
  }
}

/* ctpool task function (num_reactor_threads > 1 only): runs on a
 * dispatch_pool worker thread, one job at a time. Locks entry->dispatch_lock
 * (moved here from the poller; see _event_loop_poller_collect's own
 * comment) around the actual callback invocation(s), draining a queue
 * item's bridge eventfd first (moved from collection time specifically so
 * a _event_loop_poller_collect submission failure never silently loses a
 * queue notification; reg's own refcount, already bumped at collection
 * time, keeps bridge_efd itself alive and valid for this read regardless
 * of a concurrent event_loop_remove), then re-arms EPOLLONESHOT interest
 * via the shared helper before releasing entry's refcount.
 *
 * Ordering matters here: re-arm happens strictly BEFORE releasing entry's
 * refcount, both because the re-arm step itself needs entry to still be a
 * valid, live struct, and because _event_loop_reclaim_pending_frees (the
 * only place that ever actually frees an entry) checks refcount == 0;
 * releasing first would let a concurrent reclaim free entry out from under
 * a re-arm attempt still in flight on this thread. */
static void _event_loop_dispatch_job_fn(void *arg) {
  _dispatch_job *job = (_dispatch_job *)arg;
  struct event_loop_s *loop = job->loop;
  event_entry *entry = job->entry;

  call_once(event_loop_job_key_bundle.once, _event_loop_init_job_key);
  thread_ls_set(event_loop_job_key_bundle.key, (void *)loop);

  mutex_lock(entry->dispatch_lock);
  for (size_t i = 0; i < job->n_items; i++) {
    _dispatch_item *item = &job->items[i];
    if (!entry->is_fd) {
      _eventfd_drain(item->reg->bridge_efd);
    }
    _event_loop_run_callback(loop->self_handle, item);
    /* See _event_loop_handle_event's identical call for why (queue-backed
     * only) and why this must run before the pin release below. */
    if (!entry->is_fd) _event_loop_queue_cascade_notify_next(item->reg);
    _event_loop_release_after_dispatch(loop, item->reg);
  }
  mutex_unlock(entry->dispatch_lock);

  /* Re-arm and the refcount release happen together, under the same stripe
   * lock: _event_loop_poller_collect's own "a job is already in flight for
   * this entry" check reads entry->refcount under this exact lock, and
   * must never be able to observe "already re-armed" while refcount is
   * still nonzero; that window (re-arm done, decrement not yet visible
   * to a concurrently-collecting poller) would make the poller wrongly
   * treat a genuinely new, post-re-arm readiness event as "still in
   * flight" and silently drop it, an EPOLLONESHOT-consumed notification
   * with nothing left to ever re-arm it; a real, if quieter, bug than
   * the use-after-free this refcount check exists to prevent in the first
   * place, and not caught until reasoning through this exact ordering
   * during that fix. */
  event_loop_stripe_t *stripe = &loop->stripes[entry->stripe_idx];
  mutex_lock(stripe->lock);
  _event_loop_rearm_entry_locked(loop, entry);
  atomic_fetch_sub(&entry->refcount, 1);
  mutex_unlock(stripe->lock);

  thread_ls_set(event_loop_job_key_bundle.key, NULL);
  _mem_free(loop->m_procs, job);
}

static void *_event_loop_thread_fn(void *arg) {
  struct event_loop_s *loop = (struct event_loop_s *)arg;

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

    /* Between batches: poller_thread has fully iterated over its previous
     * batch (if any) by this point. Publish that fact by advancing
     * poller_batch_gen, then attempt to reclaim whatever deferred
     * entries/regs that advance now proves safe to free. See the large
     * comment above _event_loop_reclaim_pending_frees for the full
     * design. */
    atomic_fetch_add(&loop->poller_batch_gen, 1);
    _event_loop_reclaim_pending_frees(loop);

    int n = epoll_wait(loop->epfd, events, (int)loop->max_events_per_wait, -1);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&loop->poller_iterations_for_tests, (uint64_t)1);
#endif
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < n; i++) {
      if (loop->dispatch_pool) {
        _event_loop_poller_collect(loop, &events[i]);
      } else {
        _event_loop_handle_event(loop, &events[i]);
      }
    }
  }

  _mem_free(loop->m_procs, events);
  return NULL;
}

/* Destroys the first `created` stripes of loop->stripes (mutex + chmap each)
 * and frees the array itself, using the raw mmgmt_procs parameter (not
 * loop->m_procs) to match every other _mem_free call site in
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

/* Forward declaration: defined below event_loop_shutdown/
 * _event_loop_shutdown_internal, but needed here for
 * event_loop_create_with_mprocs's own slot-acquire-failure rollback. */
static void _event_loop_teardown_raw(struct event_loop_s *loop);

event_loop event_loop_create_with_mprocs(size_t max_events_per_wait,
                                         size_t num_lock_stripes,
                                         size_t num_reactor_threads,
                                         ccol_memmgmt_procs_t *mmgmt_procs,
                                         char **err_str) {
  if (max_events_per_wait == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("max_events_per_wait must be positive");
    return EVENT_LOOP_INVALID;
  }
  /* max_events_per_wait is narrowed to a plain `int` for epoll_wait's own
   * maxevents parameter (_event_loop_thread_fn) and used unchecked to size
   * the poller thread's events buffer allocation (max_events_per_wait *
   * sizeof(struct epoll_event), also in _event_loop_thread_fn). An
   * unreasonably large value can make that (int) cast produce a
   * non-positive maxevents (epoll_wait then fails with EINVAL) or make
   * max_events_per_wait * sizeof(struct epoll_event) overflow size_t on an
   * ILP32 platform (handing a too-small buffer to a maxevents value the
   * kernel believes is much larger); either failure is treated by
   * _event_loop_thread_fn as an ordinary, silent, permanent poller-thread
   * exit on its very first iteration, never surfaced back to this
   * constructor's own caller, leaving a handle that looks valid but never
   * dispatches anything. Rejected here instead, mirroring the
   * SIZE_MAX / sizeof(x) overflow-guard convention
   * verify_circular_queue_create_inputs already uses for the identical
   * class of hazard. */
  if (max_events_per_wait > (size_t)INT_MAX) {
    if (err_str)
      *err_str = CCOL_ERR_STR("max_events_per_wait must not exceed INT_MAX");
    return EVENT_LOOP_INVALID;
  }
  if (max_events_per_wait > SIZE_MAX / sizeof(struct epoll_event)) {
    if (err_str)
      *err_str = CCOL_ERR_STR(
          "max_events_per_wait is too large: max_events_per_wait * "
          "sizeof(struct epoll_event) would overflow size_t");
    return EVENT_LOOP_INVALID;
  }
  if (num_lock_stripes == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("num_lock_stripes must be positive");
    return EVENT_LOOP_INVALID;
  }
  if (num_reactor_threads == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("num_reactor_threads must be positive");
    return EVENT_LOOP_INVALID;
  }
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return EVENT_LOOP_INVALID;
  }

  struct event_loop_s *loop = (struct event_loop_s *)_mem_alloc(
      mmgmt_procs, sizeof(struct event_loop_s));
  if (!loop) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate memory for event_loop");
    return EVENT_LOOP_INVALID;
  }

  if (!ccol_populate_mem_mgmt_procs(loop, mmgmt_procs, err_str)) {
    _mem_free(mmgmt_procs, loop);
    return EVENT_LOOP_INVALID;
  }

  loop->epfd = epoll_create1(EPOLL_CLOEXEC);
  if (loop->epfd < 0) {
    if (err_str) *err_str = CCOL_ERR_STR("epoll_create1 failed");
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return EVENT_LOOP_INVALID;
  }

  loop->shutdown_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (loop->shutdown_efd < 0) {
    if (err_str) *err_str = CCOL_ERR_STR("eventfd failed");
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return EVENT_LOOP_INVALID;
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
    return EVENT_LOOP_INVALID;
  }

  mutex_init(loop->shutdown_lock);
  cond_var_init(loop->joined_cv);
  mutex_init(loop->reg_slot_mutex);
  loop->reg_slots =
      cvector_create_full(sizeof(event_reg_slot_t), mmgmt_procs, NULL);
  if (!loop->reg_slots) {
    if (err_str) *err_str = CCOL_ERR_STR("Failed to allocate reg slot array");
    mutex_destroy(loop->reg_slot_mutex);
    cond_var_destroy(loop->joined_cv);
    mutex_destroy(loop->shutdown_lock);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return EVENT_LOOP_INVALID;
  }
  loop->reg_free_indices =
      cvector_create_full(sizeof(uint32_t), mmgmt_procs, NULL);
  if (!loop->reg_free_indices) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate reg free-index array");
    __cvector_destroy(loop->reg_slots);
    mutex_destroy(loop->reg_slot_mutex);
    cond_var_destroy(loop->joined_cv);
    mutex_destroy(loop->shutdown_lock);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return EVENT_LOOP_INVALID;
  }
  loop->shutdown_started = false;
  loop->joined = false;
  atomic_init(&loop->shutting_down, false);
  loop->max_events_per_wait = max_events_per_wait;
  atomic_init(&loop->pending_entry_frees, NULL);
  atomic_init(&loop->pending_reg_frees, NULL);
  atomic_init(&loop->poller_batch_gen, (size_t)0);
  atomic_init(&loop->fd_generation_counter, (uint64_t)0);
  atomic_init(&loop->next_queue_stripe, (size_t)0);
  atomic_init(&loop->reg_count, (size_t)0);
#ifdef RUNNING_UNIT_TESTS
  /* Must be initialised strictly before poller_thread is created below:
   * unlike pending_resolve_count/foreign_since_fork (both initialised
   * further down, after thread_create, safely, since poller_thread never
   * touches either), this field IS written by poller_thread on every
   * single loop iteration, so atomic_init'ing it after that thread could
   * already be running would itself be a data race. */
  atomic_init(&loop->poller_iterations_for_tests, (uint64_t)0);
#endif
  loop->num_stripes = num_lock_stripes;
  loop->num_reactor_threads = num_reactor_threads;
  loop->dispatch_pool = CTPOOL_INVALID;

  loop->stripes =
      _mem_calloc(mmgmt_procs, num_lock_stripes, sizeof(event_loop_stripe_t));
  if (!loop->stripes) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate lock stripe array");
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    mutex_destroy(loop->reg_slot_mutex);
    cond_var_destroy(loop->joined_cv);
    mutex_destroy(loop->shutdown_lock);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return EVENT_LOOP_INVALID;
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
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      mutex_destroy(loop->reg_slot_mutex);
      cond_var_destroy(loop->joined_cv);
      mutex_destroy(loop->shutdown_lock);
      close(loop->shutdown_efd);
      close(loop->epfd);
      _mem_free(mmgmt_procs, loop->m_procs);
      _mem_free(mmgmt_procs, loop);
      return EVENT_LOOP_INVALID;
    }
    mutex_init(loop->stripes[stripes_created].lock);
    loop->stripes[stripes_created].queue_regs_head = NULL;
  }

  /* dispatch_pool only exists for num_reactor_threads > 1; see struct
   * event_loop_s's own field comment. Sized num_reactor_threads - 1: total
   * OS thread count stays exactly num_reactor_threads (1 poller_thread plus
   * this pool), preserving the parameter's pre-existing resource-usage
   * meaning. Unbounded queue (queue_capacity 0): the poller must never
   * block on submission (see _event_loop_poller_collect's own comment on
   * ctpool_submit's failure handling). Created before poller_thread is
   * spawned below, so dispatch_pool is always fully valid before any event
   * could possibly be submitted to it. */
  if (num_reactor_threads > 1) {
    char *pool_err = NULL;
    loop->dispatch_pool = create_cthread_pool_mp(num_reactor_threads - 1, 0,
                                                 mmgmt_procs, &pool_err);
    if (!loop->dispatch_pool) {
      if (err_str)
        *err_str = pool_err ? pool_err
                            : CCOL_ERR_STR("Failed to create dispatch pool");
      _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      mutex_destroy(loop->reg_slot_mutex);
      cond_var_destroy(loop->joined_cv);
      mutex_destroy(loop->shutdown_lock);
      close(loop->shutdown_efd);
      close(loop->epfd);
      _mem_free(mmgmt_procs, loop->m_procs);
      _mem_free(mmgmt_procs, loop);
      return EVENT_LOOP_INVALID;
    }
  }

  if (thread_create(loop->poller_thread, _event_loop_thread_fn, loop) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("pthread_create failed");
    if (loop->dispatch_pool) __ctpool_destroy(loop->dispatch_pool);
    _destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    mutex_destroy(loop->reg_slot_mutex);
    cond_var_destroy(loop->joined_cv);
    mutex_destroy(loop->shutdown_lock);
    close(loop->shutdown_efd);
    close(loop->epfd);
    _mem_free(mmgmt_procs, loop->m_procs);
    _mem_free(mmgmt_procs, loop);
    return EVENT_LOOP_INVALID;
  }

  atomic_init(&loop->pending_resolve_count, (size_t)0);
#if FORK_SAFETY_REQUIRED
  atomic_init(&loop->foreign_since_fork, false);
#endif

  /* Slot acquisition is the LITERAL LAST step, after the poller thread (and,
   * if configured, dispatch_pool) has already been successfully started;
   * mirroring chttpcli/chttpsvr's own constructors exactly, so that no
   * handle is ever exposed to any caller until this function is already
   * about to return success. A failure here must NOT be treated like the
   * ordinary allocation failures above: the poller thread is already
   * running (and may already have a dispatch_pool of its own workers too),
   * so the rollback has to actually stop them; reusing
   * _event_loop_teardown_raw (the same helper __event_loop_destroy uses)
   * does exactly that via its own call to _event_loop_shutdown_internal,
   * rather than merely freeing memory out from under a still-live thread.
   * Safe to call with no pending_resolve_count wait of any kind: no handle
   * was ever exposed to any caller at this point, so nothing could
   * possibly have resolved (and therefore pinned) it. */
  event_loop h = _event_loop_handle_slot_acquire(loop);
  if (h == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("Failed to allocate event_loop handle slot");
    _event_loop_teardown_raw(loop);
    return EVENT_LOOP_INVALID;
  }
  loop->self_handle = h;

  if (err_str) *err_str = NULL;
  return h;
}

/* The pre-existing body of what used to be the public event_loop_shutdown,
 * now taking the already-resolved raw pointer directly: called both by the
 * thin public wrapper below (after resolve/pin) and by
 * _event_loop_teardown_raw (which __event_loop_destroy and the
 * constructor's own slot-acquire-failure rollback both call without ever
 * re-resolving a handle, since marking the slot not-in-use makes any
 * further resolve of it fail). */
static ccol_retval_t _event_loop_shutdown_internal(struct event_loop_s *loop) {
  /* Self-call guard: joining poller_thread (below) from poller_thread
   * itself, or draining dispatch_pool from within one of its own worker
   * threads (ctpool_shutdown_drain has no self-join guard of its own;
   * confirmed by reading cthreadpool.c: unconditional thread_join over
   * every worker, no self-check), would each deadlock exactly like a bare
   * self-pthread_join. Applied uniformly to BOTH num_reactor_threads == 1
   * (poller_thread is the only thread, and is trivially the one running
   * whatever callback might call this) and > 1 (any of dispatch_pool's own
   * workers); leaving one configuration undefended while the other is
   * guarded would make this function's behavior on misuse surprisingly
   * dependent on how many reactor threads happen to be configured, a worse
   * API than either "always undefended" or "always defended". Returns
   * ccol_not_permitted (an existing, exact-fit enumerator, "operation not
   * allowed in current state", reused rather than minting a new one; see
   * common.h's own hard rule on enumerator numbering) instead of the
   * silent deadlock this codebase's history already paid for once (see
   * this function's own historical comment on the shutdown_efd-draining
   * bug below) rather than a comparable one. Compares against the RAW
   * pointer, not any public handle value: job->loop (what a dispatch-pool
   * worker's thread-local stashes) has always been struct event_loop_s*,
   * never the handle, so this comparison stays correct unmodified now that
   * event_loop is a uint64_t value handle. */
  call_once(event_loop_job_key_bundle.once, _event_loop_init_job_key);
  if (get_thread_id() == loop->poller_thread ||
      thread_ls_get(event_loop_job_key_bundle.key) == (void *)loop) {
    return ccol_not_permitted;
  }

  mutex_lock(loop->shutdown_lock);
  bool is_leader = !loop->shutdown_started;
  loop->shutdown_started = true;
  mutex_unlock(loop->shutdown_lock);

  if (is_leader) {
    atomic_store(&loop->shutting_down, true);

#if FORK_SAFETY_REQUIRED
    bool is_foreign = atomic_load(&loop->foreign_since_fork);
#else
    bool is_foreign = false;
#endif
    if (is_foreign) {
      /* This process inherited `loop` across a fork() call (see that
       * field's own comment): poller_thread exists here only as inert,
       * copy-on-write memory, with no execution context of its own ever
       * having existed in this process, and shutdown_efd is a real,
       * kernel-level object still shared with the still-running parent's
       * OWN, genuinely live poller thread. Skip both steps below entirely
       * rather than running them: writing shutdown_efd here would incorrectly
       * wake the PARENT's poller (a real, reproducible side effect, not
       * merely theoretical, since the underlying eventfd object is the
       * SAME one in both processes), and thread_join(poller_thread) would
       * be undefined behaviour; confirmed via a standalone reproduction
       * to reliably SIGSEGV for the exactly analogous dispatch_pool-worker
       * case (see cthreadpool.c's own foreign_since_fork field, which
       * fixes that half; ctpool_shutdown_drain below is therefore already
       * safe to call unconditionally, with no fork-awareness needed on
       * this module's own part). */
    } else {
      _eventfd_notify(loop->shutdown_efd);

      /* A single write is sufficient to wake poller_thread reliably; but
       * only because _event_loop_handle_event/_event_loop_poller_collect's
       * shared NULL-entry handling (see _event_loop_handle_event's own
       * comment) deliberately never drains shutdown_efd (see its own comment
       * for the real deadlock an earlier version of this code hit here:
       * draining it could reset the counter to 0 before epoll_wait had a
       * chance to report it, leaving the reactor stuck in epoll_wait forever
       * and this join hung on it). With the counter left permanently
       * non-zero once written, poller_thread's epoll_wait call keeps seeing
       * shutdown_efd as ready (level-triggered) until it actually returns
       * and this loop's shutting_down check (set above, before this write)
       * breaks it out. This was a real, reproduced hang during this
       * feature's own development (gdb thread-apply-all-bt on a stuck test
       * process pinned the reactor to this exact epoll_wait call), not a
       * theoretical concern; see the multi-thread shutdown tests in
       * tests/cthreadcomm/tests.c for the regression coverage this fix is
       * verified against.
       *
       * Joining poller_thread strictly before draining dispatch_pool
       * guarantees no further job can ever be submitted (only poller_thread
       * ever calls ctpool_submit), which is what makes the dispatch_pool
       * drain below safe without any separate submit-after-shutdown-started
       * race to handle: ctpool_shutdown_drain's own documented caller
       * contract ("no other thread should be submitting tasks concurrently")
       * is satisfied by construction at the point it's called. */
      thread_join(loop->poller_thread);
    }

    /* Drain (not immediate-cancel): preserves this function's own existing
     * documented contract, "no dispatch can be in flight once this
     * returns"; an immediate shutdown would cancel queued-but-not-started
     * jobs rather than run them, leaving their already-bumped reg/entry
     * refcounts in a state nothing would ever clean up. NULL for
     * num_reactor_threads == 1, where nothing was ever created to drain.
     * Safe to call unconditionally even when foreign_since_fork: this is a
     * ctpool, and cthreadpool.c's own identical fork fixup already makes
     * ctpool_shutdown_drain a join-free no-op-for-threading-purposes in
     * that case (see struct cthread_pool's own foreign_since_fork field). */
    if (loop->dispatch_pool) {
      ctpool_shutdown_drain(loop->dispatch_pool);
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

ccol_retval_t event_loop_shutdown(event_loop loop) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = _event_loop_shutdown_internal(raw);
  _event_loop_resolve_unpin(raw);
  return rv;
}

/* Shared by __event_loop_destroy (after its own poll-wait for
 * pending_resolve_count == 0 completes) and by
 * event_loop_create_with_mprocs's slot-acquire-failure rollback (called
 * with no preceding wait at all, since no handle was ever exposed to any
 * caller at that point, so pending_resolve_count is provably already 0):
 * runs shutdown (if not already started; idempotent either way via
 * _event_loop_shutdown_internal's own shutdown_lock/shutdown_started/
 * joined_cv leader/follower protocol) and then frees every remaining
 * resource. Never called on a loop any caller could still be resolving a
 * handle for; unlike ctpool's own teardown helper, event_loop's own
 * wait-ordering rule (wait BEFORE this runs, not after; see
 * __event_loop_destroy's own comment) means this helper never needs to
 * wait on pending_resolve_count itself. */
static void _event_loop_teardown_raw(struct event_loop_s *loop) {
  _event_loop_shutdown_internal(loop);

  /* Entries/regs deferred during the final round of batch processing (right
   * before shutting_down was observed) never got a chance to reach a
   * reclaim point on their own defer_gen's schedule. poller_thread and
   * every dispatch_pool worker are joined now (no concurrent access
   * possible from any of them any more), so it's safe to free every
   * remaining deferred item unconditionally rather than leaking them;
   * see _event_loop_free_all_pending's own comment for why no epoch or
   * refcount check is needed at this specific point. */
  _event_loop_free_all_pending(loop);

  /* poller_thread and every dispatch_pool worker have been joined and every
   * other in-flight event_loop_shutdown caller has already returned too
   * (the leader/follower join protocol above guarantees this); no dispatch
   * can be in flight and no other thread can be touching this loop's
   * registry. Safe to walk and free every remaining registration in every
   * stripe without any lock.
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

    event_reg_s *reg = stripe->queue_regs_head;
    while (reg) {
      event_reg_s *next = reg->loop_list_next;
      mutex_t *q_mtx;
      ccol_sel_waiter **q_head;
      ccol_sel_waiter **q_rotor;
      _queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
      _sel_unlink_waiter(&reg->waiter_node, q_head, q_rotor, q_mtx);
      mutex_destroy(reg->owning_entry->dispatch_lock);
      _mem_free(loop->m_procs, reg->owning_entry);
      _event_reg_free(loop, reg);
      reg = next;
    }

    mutex_destroy(stripe->lock);
  }
  _mem_free(loop->m_procs, loop->stripes);
  if (loop->dispatch_pool) __ctpool_destroy(loop->dispatch_pool);

  cvector_destroy(loop->reg_free_indices);
  cvector_destroy(loop->reg_slots);
  mutex_destroy(loop->reg_slot_mutex);
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

void __event_loop_destroy(event_loop loop) {
  if (!loop) return;

  /* Resolve loop through the slot table, marking the slot not-in-use in the
   * same critical section as the lookup: this is what makes a second,
   * concurrent (or later, sequential) destroy call on the same handle value
   * see a resolve failure rather than racing this call's own teardown; see
   * the slot table's own file-level comment and _event_loop_resolve's
   * comment for the full design. A stale or already-destroyed handle
   * reaching here is exactly the misuse this redesign exists to catch: it
   * is fatal, not a silent use-after-free/double-free. */
  call_once(event_loop_slot_table.once, _event_loop_slot_table_init_globals);
  call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  uint32_t idx = (uint32_t)(loop >> 32);
  uint32_t gen = (uint32_t)(loop & 0xFFFFFFFFu);
  mutex_lock(event_loop_slot_table.mutex);
  event_loop_slot_t *slot = NULL;
  struct event_loop_s *raw = NULL;
  if (idx < cvector_elem_count(event_loop_slot_table.slots)) {
    event_loop_slot_t *s =
        (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    mutex_unlock(event_loop_slot_table.mutex);
    fatal_err(
        "event_loop_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of an event_loop handle)");
  }

  /* Self-destroy-from-callback guard, the destroy-side analogue of
   * _event_loop_shutdown_internal's own self-join guard. __event_loop_destroy
   * calls that same guarded function internally (via _event_loop_teardown_raw)
   * but has no way to propagate its ccol_not_permitted return to this
   * function's void-returning, macro-driven contract; without this check,
   * a callback destroying its own loop would see the internal shutdown
   * silently skip joining anything, while this function still went on to
   * free every entry (including the one dispatch_lock the calling frame is
   * still holding locked) and the loop struct itself out from under the
   * still-executing _event_loop_handle_event/_event_loop_dispatch_job_fn
   * frame that called it; reproduced directly as a heap-use-after-free
   * before this guard was added. Checked here, before the slot is ever
   * marked not-in-use, so a caller misusing this from within a callback
   * gets the same loud, detected fatal_err() every other genuinely fatal
   * misuse of this function already gets (a stale/already-destroyed
   * handle), rather than a silent skip followed by undefined behaviour. */
  call_once(event_loop_job_key_bundle.once, _event_loop_init_job_key);
  bool is_self_call =
      (get_thread_id() == raw->poller_thread) ||
      (thread_ls_get(event_loop_job_key_bundle.key) == (void *)raw);
  if (is_self_call) {
    mutex_unlock(event_loop_slot_table.mutex);
    fatal_err(
        "event_loop_destroy: called from within a callback running on this "
        "loop's own reactor/dispatch thread (self-destroy hazard); defer "
        "destruction to another thread, or to after the callback returns, "
        "instead");
  }

  slot->in_use = false; /* blocks ALL future resolves for this handle from
                            this instant, including a second concurrent
                            destroy attempt */
  mutex_unlock(event_loop_slot_table.mutex);

  /* Wait for pending_resolve_count to reach 0 BEFORE running any teardown
   * logic at all (not just before freeing memory); see this field's own
   * struct comment for why event_loop, unlike ctpool, is safe waiting
   * first: every pin-holding
   * public entry point (event_loop_add/_modify/_pause/_resume/_remove/
   * _reg_count) is a quick, bounded, stripe-lock-only critical section that
   * never blocks waiting on the poller thread or on shutdown's own
   * broadcast machinery, so nothing here depends on shutdown running first
   * to ever release its own pin. Polling, not a condvar wait: see
   * pending_resolve_count's own field comment for why this is correct with
   * zero lost-wakeup risk, and the deliberate deviation from chttpcli/
   * chttpsvr's lock-protected-decrement pattern this represents. */
  while (atomic_load(&raw->pending_resolve_count) > 0) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000}; /* 100us */
    nanosleep(&ts, NULL);
  }

  _event_loop_teardown_raw(raw);

  /* Release the slot last, only after raw is fully torn down and freed:
   * this is what makes the slot's generation bump (and the free-index
   * push-back) mark the handle as reusable, not any earlier step. Re-fetch
   * by idx rather than reusing `slot`: a concurrent
   * event_loop_create_with_mprocs's own _event_loop_handle_slot_acquire
   * call in between may have reallocated slots' backing array via
   * cvector_push_back, invalidating any pointer into it taken before this
   * second lock acquisition; idx itself is stable. */
  mutex_lock(event_loop_slot_table.mutex);
  event_loop_slot_t *slot2 =
      (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* bumps this slot's generation past whatever value
      the just-freed loop's handle carried, so that stale handle can never
      again match a FUTURE acquire's generation for this same index */
  cvector_push_back(event_loop_slot_table.free_indices, &idx);
  mutex_unlock(event_loop_slot_table.mutex);
}

#ifdef RUNNING_UNIT_TESTS
size_t event_loop_dispatch_pool_pending_count_for_tests(event_loop loop) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw || !raw->dispatch_pool) {
    if (raw) _event_loop_resolve_unpin(raw);
    return 0;
  }
  size_t n = ctpool_pending_count(raw->dispatch_pool);
  _event_loop_resolve_unpin(raw);
  return n;
}

/* Reads how many times poller_thread has completed an epoll_wait call so
 * far. See struct event_loop_s's own poller_iterations_for_tests field
 * comment: a test samples this twice across a short, bounded window to
 * directly detect a busy-spin (the counter racing ahead by a large amount)
 * rather than relying on flaky wall-clock/CPU-usage measurement. Returns 0
 * for an invalid/stale loop handle. */
uint64_t event_loop_poller_iterations_for_tests(event_loop loop) {
  struct event_loop_s *raw = _event_loop_resolve(loop);
  if (!raw) return 0;
  uint64_t n = atomic_load(&raw->poller_iterations_for_tests);
  _event_loop_resolve_unpin(raw);
  return n;
}

/* Resolves h to its underlying struct event_loop_s* WITHOUT pinning it (does
 * not touch pending_resolve_count at all): a bare slot-table lookup, safe
 * for tests specifically because test code calling this runs synchronously,
 * single-threaded, with no concurrent destroy to race in the first place;
 * unlike _event_loop_resolve, there is no matching _unpin call a test needs
 * to remember, which would otherwise be an easy gap to leave (a forgotten
 * unpin would leave pending_resolve_count permanently nonzero on that loop,
 * silently hanging every future event_loop_destroy call against it).
 * Returns NULL under the exact same conditions _event_loop_resolve does. */
struct event_loop_s *_event_loop_resolve_for_tests(event_loop h) {
  call_once(event_loop_slot_table.once, _event_loop_slot_table_init_globals);
  call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  mutex_lock(event_loop_slot_table.mutex);
  struct event_loop_s *raw = NULL;
  if (idx < cvector_elem_count(event_loop_slot_table.slots)) {
    event_loop_slot_t *slot =
        (event_loop_slot_t *)cvector_at(event_loop_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  mutex_unlock(event_loop_slot_table.mutex);
  return raw;
}

/* Reads how many slots the event_loop handle table currently holds (grown
 * ones plus freed-but-not-yet-reused ones): lets a test assert that a
 * create/destroy churn loop reuses freed slots rather than growing the
 * table without bound. */
size_t _event_loop_slot_table_capacity_for_tests(void) {
  call_once(event_loop_slot_table.once, _event_loop_slot_table_init_globals);
  call_once(g_cthreadcomm_atfork_once, _cthreadcomm_register_atfork_once);
  mutex_lock(event_loop_slot_table.mutex);
  size_t n = cvector_elem_count(event_loop_slot_table.slots);
  mutex_unlock(event_loop_slot_table.mutex);
  return n;
}

/* Test-only hook to construct a genuinely long-held pin: resolves h (a real
 * pin, via the real _event_loop_resolve, unlike _event_loop_resolve_for_
 * tests' bare lookup), sleeps for ms milliseconds while still holding it,
 * then unpins. Every real public entry point is quick and bounded, so there
 * is no naturally-occurring slow call this module could otherwise use to
 * prove a concurrent destroy actually blocks on pending_resolve_count
 * rather than merely happening not to crash; this gives the
 * resolve_then_use_race_destroy_waits test a reliable, directly-controlled
 * way to do that. Returns false if h fails to resolve at all (nothing to
 * hold a pin on). */
bool _event_loop_resolve_pin_and_sleep_for_tests(event_loop h, int ms) {
  struct event_loop_s *raw = _event_loop_resolve(h);
  if (!raw) return false;
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
  nanosleep(&ts, NULL);
  _event_loop_resolve_unpin(raw);
  return true;
}
#endif

/* Frees the slot table's own bookkeeping arrays at process exit, so
 * make memtest's --show-leak-kinds=all does not report them as still-
 * reachable; mirrors chttpsvr.c's own _cleanup_chttpsvr_slot_table exactly
 * (see that function's own comment for the full rationale, including why
 * this is sound only given every event_loop the application created was
 * itself destroyed before process exit; the same precondition this test
 * suite already satisfies for a clean make memtest). MUST call_once here:
 * __attribute__((destructor)) functions run unconditionally for the whole
 * shared object regardless of which parts of it were actually used, so a
 * process that links this library but never creates a single event_loop
 * would otherwise lock a never-pthread_mutex_init'd mutex here. */
__attribute__((destructor)) static void _cleanup_event_loop_slot_table(void) {
  call_once(event_loop_slot_table.once, _event_loop_slot_table_init_globals);
  mutex_lock(event_loop_slot_table.mutex);
  __cvector_destroy(event_loop_slot_table.slots);
  __cvector_destroy(event_loop_slot_table.free_indices);
  mutex_unlock(event_loop_slot_table.mutex);
}
