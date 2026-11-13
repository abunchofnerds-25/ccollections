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

// Initialises cond so its timed waits measure deadlines on CLOCK_MONOTONIC.
// Every condition variable of this file that a timed wait can block on comes
// from here and every deadline from CLOCK_MONOTONIC, so a step of the wall
// clock can't stretch a timeout. Gives 0 or the first error the library
// reports.
static int init_monotonic_cond_var(ccol__cond_var *cond) {
  ccol__cond_var_attr attr;
  int rv = ccol__cond_var_attr_init(attr);
  if (rv != 0) return rv;
  rv = ccol__cond_var_attr_setclock(attr, CLOCK_MONOTONIC);
  if (rv == 0) rv = ccol__cond_var_init_ca(*cond, attr);
  ccol__cond_var_attr_destroy(attr);
  return rv;
}

// A waiter node, linked into a queue's read or write list by a cselect() call
// (for the length of the call) or by a loop registration (for its whole life).
// The list and every field here are read and changed under the owning queue's
// mutex, which keeps the nodes valid while a producer walks the list. efd is
// the eventfd of epoll mode, -1 in condition-variable mode. fwd_hops (under
// *sel_mtx) counts how many listeners the outstanding wake has visited without
// any of them moving a message; a fresh wake carries 0.
typedef struct ccol_sel_waiter {
  ccol__mutex *sel_mtx;
  ccol__cond_var *sel_cond;
  bool *ready;
  int efd;
  uint32_t fwd_hops;
  struct ccol_sel_waiter *prev;
  struct ccol_sel_waiter *next;
} ccol_sel_waiter;

#ifdef RUNNING_UNIT_TESTS
// Test-only delay in notify_waiter() between holding the queue's mutex and
// locking sel_mtx, so a test can land a fork() inside that window.
static _Atomic int g_notify_waiter_test_delay_us = 0;
void cthreadcomm__notify_waiter_test_set_delay_us(int us) {
  atomic_store(&g_notify_waiter_test_delay_us, us);
}

// Test-only count of the eventfd wakes that notify_waiter() delivers.
static _Atomic uint64_t g_notify_waiter_eventfd_wakes = 0;
uint64_t cselect__test_eventfd_wake_count(void) {
  return atomic_load_explicit(&g_notify_waiter_eventfd_wakes,
                              memory_order_relaxed);
}
#endif

// Thin wrappers over the wake descriptor of internal/cpoll.h: notify makes it
// readable and drain makes it quiet. Every eventfd of this file goes through
// them, so the file has one wake call whatever the system.
static void eventfd_notify(int efd) { ccol_wakefd_notify(efd); }

static void eventfd_drain(int efd) { ccol_wakefd_drain(efd); }

// Wakes one waiter node. Every send, receive, state change and cascade step
// wakes a listener through it. The caller holds the owning queue's mutex.
// *ready is the only record of an outstanding wake: The first notify after the
// owner cleared it sets it and delivers (an eventfd write, or a signal when the
// node has no eventfd), and later ones deliver nothing, so a busy queue costs
// one write per dispatch. The eventfd write happens under sel_mtx with the
// store to *ready, and the owner clears *ready and drains the eventfd in one
// critical section under the same mutex, so the two never disagree and no wake
// is lost. A fresh wake resets fwd_hops to 0.
static void notify_waiter(ccol_sel_waiter *w) {
#ifdef RUNNING_UNIT_TESTS
  int delay_us = atomic_load(&g_notify_waiter_test_delay_us);
  if (delay_us > 0) {
    struct timespec ts = {.tv_sec = delay_us / 1000000,
                          .tv_nsec = (long)(delay_us % 1000000) * 1000L};
    nanosleep(&ts, NULL);
  }
#endif
  ccol__mutex_lock(*w->sel_mtx);
  bool already_pending = *w->ready;
  *w->ready = true;
  w->fwd_hops = 0;
  if (!already_pending && w->efd >= 0) {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&g_notify_waiter_eventfd_wakes, (uint64_t)1,
                              memory_order_relaxed);
#endif
    eventfd_notify(w->efd);
  }
  ccol__mutex_unlock(*w->sel_mtx);
  if (!already_pending && w->efd < 0) ccol__cond_var_signal(*w->sel_cond);
}

// Like notify_waiter(), for a wake a listener forwards after a dispatch that
// moved nothing; hops is how many listeners it has visited. When a wake is
// already outstanding the smaller hop count wins, so a forward never shortens
// the walk of a wake already on its way.
static void notify_waiter_forwarded(ccol_sel_waiter *w, uint32_t hops) {
  ccol__mutex_lock(*w->sel_mtx);
  bool already_pending = *w->ready;
  *w->ready = true;
  if (!already_pending || hops < w->fwd_hops) w->fwd_hops = hops;
  if (!already_pending && w->efd >= 0) {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&g_notify_waiter_eventfd_wakes, (uint64_t)1,
                              memory_order_relaxed);
#endif
    eventfd_notify(w->efd);
  }
  ccol__mutex_unlock(*w->sel_mtx);
  if (!already_pending && w->efd < 0) ccol__cond_var_signal(*w->sel_cond);
}

// Says whether the list at head has more than k nodes, stopping after k + 1.
// The cascade step uses it to stop a forwarded wake once it has visited every
// listener. The caller holds the queue's mutex.
static bool sel_list_longer_than(const ccol_sel_waiter *head, uint32_t k) {
  uint32_t seen = 0;
  for (const ccol_sel_waiter *w = head; w != NULL; w = w->next) {
    if (seen == k) return true;
    seen++;
  }
  return false;
}

// Wakes one waiter of a list and moves *rotor on, so successive calls rotate
// through every linked waiter. It's for a message or a free slot, where waking
// more than one is a thundering herd. *rotor is NULL (start at *head) or a node
// of this list; sel_unlink_waiter_locked keeps that true. The rotation matters
// because a loop registration stays linked and new waiters prepend: Without it
// the newest registration would get every notify and an older one whose sibling
// keeps the queue drained would never be called. The caller holds the queue's
// mutex.
static void notify_one_sel_waiter(ccol_sel_waiter **head,
                                  ccol_sel_waiter **rotor) {
  ccol_sel_waiter *target = *rotor ? *rotor : *head;
  if (!target) return;
  notify_waiter(target);
  *rotor = target->next;
}

// Wakes every waiter of a list, for disable_sending and enable_sending, after
// which all of them must look at the state again. The caller holds the queue's
// mutex.
static void notify_all_sel_waiters(ccol_sel_waiter *head) {
  for (ccol_sel_waiter *w = head; w != NULL; w = w->next) notify_waiter(w);
}

// The thread-specific key that holds the queue whose loop callback runs on the
// calling thread, or NULL. One key answers that for every queue. live says the
// key exists: Creation can fail, and a value set on a key we don't own would
// reach another component's destructor. ceventloop_create_mp refuses to build a
// loop while live is false, so only an existing key is ever set. The key is
// deleted when the loop slot table is released, so dlopen() and dlclose()
// cycles don't use up keys.
static struct {
  ccol__thread_ls_key key;
  ccol__once_flag once;
  atomic_bool live;
} queue_dispatch_marker_bundle = {.once = CCOL__ONCE_INIT};

// Creates the marker key and records whether that worked. It runs once,
// through queue_dispatch_marker_bundle.once, before the key is first needed.
static void queue_dispatch_marker_init(void) {
  if (ccol__thread_ls_key_create(queue_dispatch_marker_bundle.key, NULL) == 0)
    atomic_store(&queue_dispatch_marker_bundle.live, true);
}

// True when the marker key exists. ceventloop_create_mp asks this first.
static bool queue_dispatch_marker_available(void) {
  ccol__call_once(queue_dispatch_marker_bundle.once,
                  queue_dispatch_marker_init);
  return atomic_load(&queue_dispatch_marker_bundle.live);
}

#ifdef RUNNING_UNIT_TESTS
// Test-only count of upcoming thread mark stores that fail as without memory.
static _Atomic uint32_t g_mark_set_failures = 0;

// Test-only: Arms the failing mark stores.
void ceventloop__test_fail_next_marks(uint32_t count) {
  atomic_store(&g_mark_set_failures, count);
}
#endif

// Stores value on key for the calling thread and gives 0 or an error number.
// Every thread mark of the module goes through it, so a test can make the store
// fail as it can for real when the C library can't allocate the thread's slot.
static int cthreadcomm_mark_set(ccol__thread_ls_key key, void *value) {
#ifdef RUNNING_UNIT_TESTS
  uint32_t left = atomic_load(&g_mark_set_failures);
  while (left > 0) {
    if (atomic_compare_exchange_weak(&g_mark_set_failures, &left, left - 1))
      return ENOMEM;
  }
#endif
  return ccol__thread_ls_set(key, value);
}

// The queue mark of a thread whose key store failed. A mark tries the key
// first, the thread clears this copy itself, and a check reads both. Without it
// a destroy from a callback would wait for itself instead of being reported.
static __thread const void *queue_dispatch_mark_fallback;

// Marks the calling thread as running queue's loop callback, so that a destroy
// of the queue from that callback is caught instead of waiting for itself. Only
// a live loop's dispatch calls it, so the key exists. Gives true when the
// fallback copy holds the mark; pass that to queue_dispatch_unmark().
static bool queue_dispatch_mark(void *queue) {
  ccol__call_once(queue_dispatch_marker_bundle.once,
                  queue_dispatch_marker_init);
  if (cthreadcomm_mark_set(queue_dispatch_marker_bundle.key, queue) == 0)
    return false;
  queue_dispatch_mark_fallback = queue;
  return true;
}

// Clears the queue mark when the callback returns, so the thread's later work
// stays unmarked. Setting NULL on the key needs no storage and can't fail.
static void queue_dispatch_unmark(bool fell_back) {
  if (fell_back)
    queue_dispatch_mark_fallback = NULL;
  else
    (void)ccol__thread_ls_set(queue_dispatch_marker_bundle.key, NULL);
}

// Says whether the calling thread runs a callback of queue; the destroy wait
// asks it to catch a destroy from that callback. Without the key no loop
// exists, so the answer is false.
static bool queue_dispatch_marker_is(const void *queue) {
  if (!queue_dispatch_marker_available()) return false;
  return ccol__thread_ls_get(queue_dispatch_marker_bundle.key) == queue ||
         queue_dispatch_mark_fallback == queue;
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: Whether the calling thread runs a callback of queue, the answer
// the queue destroy uses.
bool ceventloop__test_dispatches_queue(const void *queue) {
  return queue_dispatch_marker_is(queue);
}
#endif

// Waits until no loop dispatch holds a reference to the queue; a queue destroy
// calls it after the caller drained the queue and removed its registrations.
// ceventloop_remove doesn't wait for a callback already running, so such a
// callback can still be inside the queue, and freeing the queue under it is a
// use-after-free. Only a running dispatch holds a reference (see
// queue_dispatch_ref_acquire). This thread holds no lock here, so the wait
// can't close a lock cycle. A destroy from that same callback would wait for
// itself and is a fatal error. It polls a single atomic with short sleeps,
// since a signal would cost every dispatch a condition variable signal for a
// wait that is cold.
static void queue_wait_for_dispatch_refs(_Atomic size_t *refs,
                                         const void *queue) {
  if (atomic_load(refs) == 0) return;

  if (queue_dispatch_marker_is(queue)) {
    ccol__fatal_err(
        "a queue must not be destroyed from inside an ceventloop "
        "callback dispatching that same queue (queue=%p)",
        queue);
  }

  while (atomic_load(refs) != 0) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 200000L};
    nanosleep(&ts, NULL);
  }
}

// Queue mutex fork safety. A fork() while another thread holds a queue's mutex
// leaves that mutex locked for ever in the child. This registry keeps the mutex
// of every live ccircq and cdynmq (a cchan is two ccircqs), so the fork
// handlers can lock them all before fork() and unlock them after. A queue owns
// no thread and no kernel object, so after the unlock its state is valid in the
// child. The registry has no fork handlers of its own: Two independent sets
// would run in an order that depends on which subsystem the process used first,
// and could lock a queue's mutex after a registration's wait_mtx, the reverse
// of notify_waiter's order. One merged set, cthreadcomm_atfork_prepare and its
// release functions, locks each queue's mutex after the stripe locks that own
// its registrations and before their wait_mtx, which matches every nesting in
// this file.
#if CCOL_FORK_SAFETY_REQUIRED
// One registry entry: A live queue's mutex and the address of that queue's
// registry_idx, which always holds the entry's position.
typedef struct {
  ccol__mutex *mutex;
  size_t *index;
} queue_mutex_registry_entry;

// The registry. addrs is an unordered vector, so a remove moves the last entry
// into the hole in O(1); it's NULL once the exit destructor released it.
// release_deferred is set when that destructor found a live queue, and the
// remove of the last entry then does the release.
static struct {
  ccol__mutex mutex;
  ccol__once_flag once;
  cvec addrs;
  bool release_deferred;
} queue_mutex_registry = {0};
#endif

// Guards the one merged ccol__at_fork() registration for the queue registry
// and the loop slot table. It's here so the registry can register the handlers
// itself, in a process that never touches ceventloop.
static ccol__once_flag g_cthreadcomm_atfork_once = CCOL__ONCE_INIT;

static void cthreadcomm_register_atfork_once(void);

#if CCOL_FORK_SAFETY_REQUIRED
// Creates the registry's mutex and vector on first use, before anything adds,
// removes or walks entries. A failure is fatal.
static void queue_mutex_registry_init_globals(void) {
  if (ccol__mutex_init(queue_mutex_registry.mutex) != 0)
    ccol__fatal_err("queue mutex registry: failed to initialize mutex");
  queue_mutex_registry.addrs =
      cvector_create(sizeof(queue_mutex_registry_entry), NULL);
  if (!queue_mutex_registry.addrs)
    ccol__fatal_err("queue mutex registry: failed to allocate address vector");
}

// Registers m, with the queue's registry_idx at index, so a fork() locks it.
// Each queue calls it once, after it's fully built. Gives false when the vector
// can't grow, or after the exit destructor released the registry; the queue
// constructor treats that as an allocation failure. Takes the registry mutex.
static bool queue_mutex_registry_add(ccol__mutex *m, size_t *index) {
  ccol__call_once(queue_mutex_registry.once, queue_mutex_registry_init_globals);
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
  ccol__mutex_lock(queue_mutex_registry.mutex);
  bool ok = false;
  if (queue_mutex_registry.addrs != NULL) {
    queue_mutex_registry_entry e = {.mutex = m, .index = index};
    *index = cvector_elem_count(queue_mutex_registry.addrs);
    ok = cvector_push_back(queue_mutex_registry.addrs, &e) == ccol_success;
  }
  ccol__mutex_unlock(queue_mutex_registry.mutex);
  return ok;
}

#ifdef RUNNING_UNIT_TESTS
// Test-only count of the entries removes examine.
static _Atomic uint64_t g_queue_registry_remove_probes_for_tests = 0;
#endif

// Unregisters m, so the fork handlers never lock a destroyed mutex; the queue
// destroy calls it before it destroys the mutex. The last entry moves into m's
// position (and that queue's registry_idx is updated), since the fork walk
// needs no order. An entry at *index that isn't m is left alone. When the exit
// destructor deferred the release, removing the last entry frees the vector.
// Takes the registry mutex.
static void queue_mutex_registry_remove(ccol__mutex *m, size_t *index) {
  ccol__call_once(queue_mutex_registry.once, queue_mutex_registry_init_globals);
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
  ccol__mutex_lock(queue_mutex_registry.mutex);
  if (!queue_mutex_registry.addrs) {
    ccol__mutex_unlock(queue_mutex_registry.mutex);
    return;
  }
  size_t n = cvector_elem_count(queue_mutex_registry.addrs);
  size_t i = *index;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&g_queue_registry_remove_probes_for_tests,
                            (uint64_t)1, memory_order_relaxed);
#endif
  if (i < n &&
      ((queue_mutex_registry_entry *)cvector_at(queue_mutex_registry.addrs, i))
              ->mutex == m) {
    queue_mutex_registry_entry last;
    cvector_pop_back(queue_mutex_registry.addrs, &last);
    if (i != n - 1) {
      *(queue_mutex_registry_entry *)cvector_at(queue_mutex_registry.addrs, i) =
          last;
      *last.index = i;
    }
  }
  if (queue_mutex_registry.release_deferred &&
      cvector_elem_count(queue_mutex_registry.addrs) == 0) {
    cvector_destroy(queue_mutex_registry.addrs);
    queue_mutex_registry.release_deferred = false;
  }
  ccol__mutex_unlock(queue_mutex_registry.mutex);
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: Checks that every entry's registry_idx names its position, and
// gives the entry count and the probes removes made, so a test can prove a
// remove is O(1).
bool ccol__queue_mutex_registry_check_for_tests(size_t *count,
                                                uint64_t *remove_probes) {
  ccol__call_once(queue_mutex_registry.once, queue_mutex_registry_init_globals);
  ccol__mutex_lock(queue_mutex_registry.mutex);
  size_t n = queue_mutex_registry.addrs
                 ? cvector_elem_count(queue_mutex_registry.addrs)
                 : 0;
  bool ok = true;
  for (size_t k = 0; k < n; k++) {
    queue_mutex_registry_entry *e =
        (queue_mutex_registry_entry *)cvector_at(queue_mutex_registry.addrs, k);
    if (*e->index != k) ok = false;
  }
  ccol__mutex_unlock(queue_mutex_registry.mutex);
  if (count) *count = n;
  if (remove_probes)
    *remove_probes = atomic_load(&g_queue_registry_remove_probes_for_tests);
  return ok;
}
#endif

// Frees the registry's vector at process exit, so valgrind doesn't report it.
// While a queue is still registered it only sets release_deferred, and the
// remove of the last entry frees the vector. The mutex stays, since the fork
// handlers can still take it.
__attribute__((destructor)) static void cleanup_queue_mutex_registry(void) {
  ccol__call_once(queue_mutex_registry.once, queue_mutex_registry_init_globals);
  ccol__mutex_lock(queue_mutex_registry.mutex);
  if (queue_mutex_registry.addrs) {
    if (cvector_elem_count(queue_mutex_registry.addrs) > 0)
      queue_mutex_registry.release_deferred = true;
    else
      cvector_destroy(queue_mutex_registry.addrs);
  }
  ccol__mutex_unlock(queue_mutex_registry.mutex);
}
#endif

// Stores the error of a failed condition wait in errno, as the timed calls
// promise. It's out of line so the timed functions keep no register for it.
static __attribute__((noinline, cold)) void ccol_store_wait_errno(int err) {
  errno = err;
}

// Circular queue

// The circular queue. Every field is guarded by mutex except dispatch_refs,
// which is atomic and sits last, away from what send and receive read.
struct ccircq {
  ccol__mutex mutex;
  ccol__cond_var read_cond;
  ccol__cond_var write_cond;

  // Threads blocked on read_cond and write_cond. A send or receive signals only
  // when the count isn't zero, so every wait site, timed ones included, keeps
  // them.
  size_t readers_waiting;
  size_t writers_waiting;

  size_t read_index;
  size_t write_index;
  size_t max_size;
  size_t msg_count;

  ccol_memmgmt_procs *m_procs;

  cmessage *msg_array;
  bool writing_disabled;

  ccol_sel_waiter *sel_read_waiters_head;
  ccol_sel_waiter *sel_write_waiters_head;

  // Rotation cursors of notify_one_sel_waiter(); NULL starts at the head.
  ccol_sel_waiter *sel_read_rotor;
  ccol_sel_waiter *sel_write_rotor;
  // Registrations of shut-down loops that still name this queue although their
  // nodes left the lists. A destroy refuses the queue while it isn't zero.
  size_t detached_loop_regs;

  // Loop dispatches running a callback on this queue right now; the destroy
  // waits for it to reach zero.
  _Atomic size_t dispatch_refs;

#if CCOL_FORK_SAFETY_REQUIRED
  // Position in queue_mutex_registry, under the registry mutex.
  size_t registry_idx;
#endif
};

// Checks the arguments of ccircq_create_mp() before it allocates anything:
// max_size from 1 to ccol_max_elem_count and small enough that
// max_size * sizeof(cmessage) doesn't overflow (the array is allocated with a
// plain multiplication), and a well-formed allocator. Sets *err_str on
// failure.
static bool verify_circular_queue_create_inputs(size_t max_size,
                                                ccol_memmgmt_procs *mmgmt_procs,
                                                char **err_str) {
  if (max_size == 0) {
    if (err_str) {
      *err_str = CCOL__ERR_STR("max_size should be positive");
    }
    return false;
  }

  if (max_size > ccol_max_elem_count) {
    if (err_str) {
      *err_str = CCOL__ERR_STR("max_size can not exceed ccol_max_elem_count");
    }
    return false;
  }

  if (max_size > SIZE_MAX / sizeof(cmessage)) {
    if (err_str) {
      *err_str = CCOL__ERR_STR(
          "max_size is too large: max_size * sizeof(cmessage) would "
          "overflow size_t");
    }
    return false;
  }

  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return false;
  }

  return true;
}

// Creates a circular queue, the constructor behind ccircq_create() and both
// directions of a cchan: The struct, its message array, the mutex and the two
// monotonic condition variables, with sends on. Gives the queue, or NULL with
// *err_str set. Registering the mutex for fork safety is the last step, so its
// failure unwinds like any allocation failure.
ccircq *ccircq_create_mp(size_t max_size, ccol_memmgmt_procs *mmgmt_procs,
                         char **err_str) {
  if (!verify_circular_queue_create_inputs(max_size, mmgmt_procs, err_str)) {
    return NULL;
  }

  ccircq *cq = (ccircq *)ccol__mem_alloc(mmgmt_procs, sizeof(ccircq));
  if (!cq) {
    if (err_str) {
      *err_str = CCOL__ERR_STR("Failed to allocate memory for ccircq");
    }
    return NULL;
  }

  if (!ccol__populate_mem_mgmt_procs(cq, mmgmt_procs, err_str)) {
    ccol__mem_free(mmgmt_procs, cq);
    return NULL;
  }

  cq->msg_array =
      (cmessage *)ccol__mem_alloc(mmgmt_procs, max_size * sizeof(cmessage));
  if (!cq->msg_array) {
    if (err_str) {
      *err_str = CCOL__ERR_STR("Failed to allocate memory for cq msg_array");
    }
    ccol__mem_free(mmgmt_procs, cq->m_procs);
    ccol__mem_free(mmgmt_procs, cq);
    return NULL;
  }

  if (ccol__mutex_init(cq->mutex) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("Failed to initialize cq mutex");
    ccol__mem_free(mmgmt_procs, cq->msg_array);
    ccol__mem_free(mmgmt_procs, cq->m_procs);
    ccol__mem_free(mmgmt_procs, cq);
    return NULL;
  }
  if (init_monotonic_cond_var(&cq->read_cond) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("Failed to initialize cq read_cond");
    ccol__mutex_destroy(cq->mutex);
    ccol__mem_free(mmgmt_procs, cq->msg_array);
    ccol__mem_free(mmgmt_procs, cq->m_procs);
    ccol__mem_free(mmgmt_procs, cq);
    return NULL;
  }
  if (init_monotonic_cond_var(&cq->write_cond) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("Failed to initialize cq write_cond");
    ccol__mutex_destroy(cq->mutex);
    ccol__cond_var_destroy(cq->read_cond);
    ccol__mem_free(mmgmt_procs, cq->msg_array);
    ccol__mem_free(mmgmt_procs, cq->m_procs);
    ccol__mem_free(mmgmt_procs, cq);
    return NULL;
  }
  cq->read_index = 0;
  cq->write_index = 0;
  cq->max_size = max_size;
  cq->msg_count = 0;
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
  if (!queue_mutex_registry_add(&cq->mutex, &cq->registry_idx)) {
    if (err_str) {
      *err_str =
          CCOL__ERR_STR("Failed to register ccircq's mutex for fork safety");
    }
    ccol__mutex_destroy(cq->mutex);
    ccol__cond_var_destroy(cq->read_cond);
    ccol__cond_var_destroy(cq->write_cond);
    ccol__mem_free(mmgmt_procs, cq->msg_array);
    ccol__mem_free(mmgmt_procs, cq->m_procs);
    ccol__mem_free(mmgmt_procs, cq);
    return NULL;
  }
#endif

  if (err_str) {
    *err_str = NULL;
  }

  return cq;
}

// Destroys a circular queue; ccircq_destroy() and cchan__destroy() call it.
// It asserts when a waiter is linked or a detached registration still names the
// queue (checked under the mutex), since the unlink of such a node later reads
// the queue. Then it waits for running dispatches, and only after that asserts
// on queued messages, because a write callback can still send in that time. The
// mutex leaves the fork registry before it's destroyed.
void ccircq__destroy(ccircq *cq) {
  if (cq) {
    ccol__mutex_lock(cq->mutex);
    bool has_sel_waiters = (cq->sel_read_waiters_head != NULL) ||
                           (cq->sel_write_waiters_head != NULL) ||
                           cq->detached_loop_regs != 0;
    ccol__mutex_unlock(cq->mutex);
    if (has_sel_waiters) {
      ccol__assert(false);
    }

    queue_wait_for_dispatch_refs(&cq->dispatch_refs, cq);

    if (ccircq_msg_count(cq) > 0) {
      ccol__assert(false);
    }

    if (cq->msg_array) {
      ccol__mem_free(cq->m_procs, cq->msg_array);
      cq->msg_array = NULL;
    }

#if CCOL_FORK_SAFETY_REQUIRED
    queue_mutex_registry_remove(&cq->mutex, &cq->registry_idx);
#endif

    ccol__mutex_destroy(cq->mutex);
    ccol__cond_var_destroy(cq->read_cond);
    ccol__cond_var_destroy(cq->write_cond);

    if (cq->m_procs) {
      ccol_free free_func = cq->m_procs->free;
      free_func(cq->m_procs);
      free_func(cq);
    } else {
      ccol__default_mem_free(cq);
    }
  }
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: Locks and unlocks cq's mutex directly.
void ccircq__test_lock_mutex_for_tests(ccircq *cq) {
  ccol__mutex_lock(cq->mutex);
}

void ccircq__test_unlock_mutex_for_tests(ccircq *cq) {
  ccol__mutex_unlock(cq->mutex);
}

// Test-only: Whether a cselect() waiter is linked into the read list, read
// under the mutex, so a test can wait for one in a bounded loop.
bool ccircq__test_has_sel_read_waiter_for_tests(ccircq *cq) {
  ccol__mutex_lock(cq->mutex);
  bool has_waiter = cq->sel_read_waiters_head != NULL;
  ccol__mutex_unlock(cq->mutex);
  return has_waiter;
}

// Test-only: Counts the waiters of the read list under the mutex.
size_t ccircq__test_sel_read_waiter_count_for_tests(ccircq *cq) {
  ccol__mutex_lock(cq->mutex);
  size_t n = 0;
  for (ccol_sel_waiter *w = cq->sel_read_waiters_head; w; w = w->next) ++n;
  ccol__mutex_unlock(cq->mutex);
  return n;
}

// Test-only: The blocked reader and writer counts, read under the mutex, so a
// test can wait until a thread really parked.
size_t ccircq__waiting_readers_for_tests(ccircq *cq) {
  ccol__mutex_lock(cq->mutex);
  size_t n = cq->readers_waiting;
  ccol__mutex_unlock(cq->mutex);
  return n;
}

size_t ccircq__waiting_writers_for_tests(ccircq *cq) {
  ccol__mutex_lock(cq->mutex);
  size_t n = cq->writers_waiting;
  ccol__mutex_unlock(cq->mutex);
  return n;
}
#endif

// Stores msg at write_index, takes msg->data from the sender (the zero-copy
// hand-over), counts it and wakes one reader and one read waiter. Every send
// path of the circular queue ends here. The caller holds the mutex and has
// checked that there is room.
static void sendto_cq(ccircq *cq, cmessage *msg) {
  cq->msg_array[cq->write_index].data = msg->data;
  cq->msg_array[cq->write_index++].size = (msg->data == NULL) ? 0 : msg->size;
  msg->data = NULL;
  if (cq->write_index == cq->max_size) {
    cq->write_index = 0;
  }
  ++cq->msg_count;

  if (cq->readers_waiting) ccol__cond_var_signal(cq->read_cond);
  notify_one_sel_waiter(&cq->sel_read_waiters_head, &cq->sel_read_rotor);
}

// Checks the arguments of every circular queue send: non-NULL queue and
// message, and data and size that agree (a pointer needs a size, NULL needs
// 0).
static bool verify_circq_send_zc_params(ccircq *cq, cmessage *msg) {
  if (!cq || !msg || (msg->size == 0 && msg->data != NULL) ||
      (msg->data == NULL && msg->size != 0)) {
    return false;
  }

  return true;
}

// Sends msg, blocking on write_cond while the queue is full; it's the send for
// a producer that should wait for room (backpressure). Gives
// ccol_not_permitted when sends are off, checked before and after the wait.
ccol_retval ccircq_send(ccircq *cq, cmessage *msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  ccol__mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    ccol__mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  while (cq->msg_count == cq->max_size && !cq->writing_disabled) {
    ++cq->writers_waiting;
    ccol__cond_var_wait(cq->write_cond, cq->mutex);
    --cq->writers_waiting;
  }

  if (cq->writing_disabled) {
    ccol__mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  sendto_cq(cq, msg);

  ccol__mutex_unlock(cq->mutex);

  return ccol_success;
}

// Sends msg when there's room and gives ccol_container_full otherwise, for a
// producer that must never block. Gives ccol_not_permitted when sends are
// off.
ccol_retval ccircq_try_send(ccircq *cq, cmessage *msg) {
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  ccol_retval result = ccol_container_full;

  ccol__mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    ccol__mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  if (cq->msg_count < cq->max_size) {
    sendto_cq(cq, msg);
    result = ccol_success;
  }

  ccol__mutex_unlock(cq->mutex);

  return result;
}

static void recvfrom_cq(ccircq *cq, cmessage *target_buf);

#ifdef RUNNING_UNIT_TESTS
// Test-only state of the forced wait error of ccircq_timed_send: The armed
// flag, the "also race" flag, and the message that the imitated concurrent
// receive took out.
static _Atomic bool g_circq_send_force_condvar_wait_error = false;
static _Atomic bool g_circq_send_force_condvar_wait_error_also_race = false;
static cmessage g_circq_send_race_freed_msg = {0};

// Test-only: Arms the forced wait error of the next timed send.
void ccircq__test_force_next_send_condvar_wait_error(void) {
  atomic_store(&g_circq_send_force_condvar_wait_error, true);
}

// Test-only: Arms the forced wait error together with an imitated receive that
// frees a slot under the same mutex.
void ccircq__test_force_next_send_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_circq_send_force_condvar_wait_error, true);
  atomic_store(&g_circq_send_force_condvar_wait_error_also_race, true);
}

// Test-only: Hands out, once, the message the imitated receive took.
cmessage ccircq__test_take_race_freed_msg(void) {
  cmessage m = g_circq_send_race_freed_msg;
  g_circq_send_race_freed_msg = (cmessage){0};
  return m;
}
#endif

// Sends msg, waiting at most timeout_us for room, for a producer that may wait
// but not for ever (0 is ccircq_try_send()). The deadline is computed once
// before the loop, so a spurious wake can't stretch it. Its wait is counted in
// writers_waiting like the untimed one, or a receiver would never signal it.
// After a failed wait it looks at the queue again before giving up, since a
// receiver can free a slot just before an unrelated error or the timeout is
// seen; a free slot or turned-off sends win over the error.
ccol_retval ccircq_timed_send(ccircq *cq, cmessage *msg, uint64_t timeout_us) {
  if (timeout_us == 0) return ccircq_try_send(cq, msg);
  if (!verify_circq_send_zc_params(cq, msg)) {
    return ccol_invalid_args;
  }

  ccol__mutex_lock(cq->mutex);

  if (cq->writing_disabled) {
    ccol__mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  if (cq->msg_count == cq->max_size) {
    int retval;
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);

    while (cq->msg_count == cq->max_size && !cq->writing_disabled) {
      ++cq->writers_waiting;
#ifdef RUNNING_UNIT_TESTS
      if (atomic_load(&g_circq_send_force_condvar_wait_error)) {
        atomic_store(&g_circq_send_force_condvar_wait_error, false);
        retval = EINVAL;
        if (atomic_load(&g_circq_send_force_condvar_wait_error_also_race)) {
          atomic_store(&g_circq_send_force_condvar_wait_error_also_race, false);
          recvfrom_cq(cq, &g_circq_send_race_freed_msg);
        }
      } else {
        retval = ccol__cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
      }
#else
      retval = ccol__cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
#endif
      --cq->writers_waiting;
      if (retval) {
        if (cq->msg_count < cq->max_size || cq->writing_disabled) break;
        if (retval != ETIMEDOUT) {
          ccol_store_wait_errno(retval);
          ccol__mutex_unlock(cq->mutex);
          return ccol_unexpected_failure;
        }
        ccol__mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
  }

  if (cq->writing_disabled) {
    ccol__mutex_unlock(cq->mutex);
    return ccol_not_permitted;
  }

  sendto_cq(cq, msg);

  ccol__mutex_unlock(cq->mutex);

  return ccol_success;
}

// Sends msg whether sends are on or off (see internal/cqueueowner.h), waiting
// at most timeout_us for room with the same wait as the timed send. 0 gives
// ccol_container_full on a full queue.
ccol_retval ccircq_owner_send(ccircq *cq, cmessage *msg, uint64_t timeout_us) {
  if (!verify_circq_send_zc_params(cq, msg)) return ccol_invalid_args;
  ccol__mutex_lock(cq->mutex);
  if (cq->msg_count == cq->max_size) {
    if (timeout_us == 0) {
      ccol__mutex_unlock(cq->mutex);
      return ccol_container_full;
    }
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);
    while (cq->msg_count == cq->max_size) {
      ++cq->writers_waiting;
      int retval =
          ccol__cond_var_timedwait(cq->write_cond, cq->mutex, abs_time);
      --cq->writers_waiting;
      if (retval && cq->msg_count == cq->max_size) {
        if (retval != ETIMEDOUT) ccol_store_wait_errno(retval);
        ccol__mutex_unlock(cq->mutex);
        return retval == ETIMEDOUT ? ccol_timed_out : ccol_unexpected_failure;
      }
    }
  }
  sendto_cq(cq, msg);
  ccol__mutex_unlock(cq->mutex);
  return ccol_success;
}

// Takes the message at read_index into target_buf and wakes one blocked sender
// and one write waiter. Every receive path of the circular queue ends here,
// and the owner send's test hook uses it to imitate a receive. The caller holds
// the mutex and has checked that a message is queued.
static void recvfrom_cq(ccircq *cq, cmessage *target_buf) {
  target_buf->data = cq->msg_array[cq->read_index].data;
  target_buf->size = cq->msg_array[cq->read_index++].size;
  if (cq->read_index == cq->max_size) {
    cq->read_index = 0;
  }

  --cq->msg_count;

  if (cq->writers_waiting) ccol__cond_var_signal(cq->write_cond);
  notify_one_sel_waiter(&cq->sel_write_waiters_head, &cq->sel_write_rotor);
}

// Checks the arguments of every circular queue receive: Neither the queue nor
// the buffer may be NULL.
static bool verify_recvfrom_cq_zc_params(ccircq *cq, cmessage *target_buf) {
  if (!cq || !target_buf) {
    return false;
  }

  return true;
}

// Receives a message, blocking on read_cond while the queue is empty, for a
// consumer that has nothing else to do.
ccol_retval ccircq_recv(ccircq *cq, cmessage *target_buf) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol__mutex_lock(cq->mutex);

  while (cq->msg_count == 0) {
    ++cq->readers_waiting;
    ccol__cond_var_wait(cq->read_cond, cq->mutex);
    --cq->readers_waiting;
  }

  recvfrom_cq(cq, target_buf);

  ccol__mutex_unlock(cq->mutex);

  return ccol_success;
}

// Receives a message or gives ccol_container_empty at once, for a consumer that
// must not block (an event loop callback, or after cselect()).
ccol_retval ccircq_try_recv(ccircq *cq, cmessage *target_buf) {
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_retval result = ccol_container_empty;

  ccol__mutex_lock(cq->mutex);

  if (cq->msg_count > 0) {
    result = ccol_success;
    recvfrom_cq(cq, target_buf);
  }

  ccol__mutex_unlock(cq->mutex);

  return result;
}

#ifdef RUNNING_UNIT_TESTS
// Test-only flags of the forced wait error of ccircq_timed_recv.
static _Atomic bool g_circq_recv_force_condvar_wait_error = false;
static _Atomic bool g_circq_recv_force_condvar_wait_error_also_race = false;

// Test-only: Arms the forced wait error of the next timed receive.
void ccircq__test_force_next_recv_condvar_wait_error(void) {
  atomic_store(&g_circq_recv_force_condvar_wait_error, true);
}

// Test-only: Arms the forced wait error together with an imitated send of a
// sentinel under the same mutex.
void ccircq__test_force_next_recv_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_circq_recv_force_condvar_wait_error, true);
  atomic_store(&g_circq_recv_force_condvar_wait_error_also_race, true);
}
#endif

// Receives a message, waiting at most timeout_us against one deadline, for a
// consumer that also has other work (0 is ccircq_try_recv()). As in the timed
// send, a failed wait looks at the queue again, and a message that arrived wins
// over the error or the timeout.
ccol_retval ccircq_timed_recv(ccircq *cq, cmessage *target_buf,
                              uint64_t timeout_us) {
  if (timeout_us == 0) return ccircq_try_recv(cq, target_buf);
  if (!verify_recvfrom_cq_zc_params(cq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol__mutex_lock(cq->mutex);

  if (cq->msg_count == 0) {
    int retval;
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);

    while (cq->msg_count == 0) {
      ++cq->readers_waiting;
#ifdef RUNNING_UNIT_TESTS
      if (atomic_load(&g_circq_recv_force_condvar_wait_error)) {
        atomic_store(&g_circq_recv_force_condvar_wait_error, false);
        retval = EINVAL;
        if (atomic_load(&g_circq_recv_force_condvar_wait_error_also_race)) {
          atomic_store(&g_circq_recv_force_condvar_wait_error_also_race, false);
          cmessage sentinel = {.data = NULL, .size = 0};
          sendto_cq(cq, &sentinel);
        }
      } else {
        retval = ccol__cond_var_timedwait(cq->read_cond, cq->mutex, abs_time);
      }
#else
      retval = ccol__cond_var_timedwait(cq->read_cond, cq->mutex, abs_time);
#endif
      --cq->readers_waiting;
      if (retval) {
        if (cq->msg_count > 0) break;
        if (retval != ETIMEDOUT) {
          ccol_store_wait_errno(retval);
          ccol__mutex_unlock(cq->mutex);
          return ccol_unexpected_failure;
        }
        ccol__mutex_unlock(cq->mutex);
        return ccol_timed_out;
      }
    }
  }

  recvfrom_cq(cq, target_buf);

  ccol__mutex_unlock(cq->mutex);

  return ccol_success;
}

// Turns sends off, so an owner can stop new work while it drains the queue.
// Under the mutex it wakes every blocked sender (which then gives
// ccol_not_permitted) and every write waiter.
ccol_retval ccircq_disable_sending(ccircq *cq) {
  if (cq) {
    ccol__mutex_lock(cq->mutex);
    cq->writing_disabled = true;
    ccol__cond_var_broadcast(cq->write_cond);
    notify_all_sel_waiters(cq->sel_write_waiters_head);
    ccol__mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

// Turns sends back on after ccircq_disable_sending(), and under the mutex
// wakes every blocked sender and every write waiter.
ccol_retval ccircq_enable_sending(ccircq *cq) {
  if (cq) {
    ccol__mutex_lock(cq->mutex);
    cq->writing_disabled = false;
    ccol__cond_var_broadcast(cq->write_cond);
    notify_all_sel_waiters(cq->sel_write_waiters_head);
    ccol__mutex_unlock(cq->mutex);
    return ccol_success;
  }
  return ccol_invalid_args;
}

// Gives the message count, read under the mutex, or ccol_invalid_size for
// NULL. The destroy uses it, and so can a caller that wants a snapshot.
size_t ccircq_msg_count(ccircq *cq) {
  size_t result = ccol_invalid_size;

  if (cq) {
    ccol__mutex_lock(cq->mutex);
    result = cq->msg_count;
    ccol__mutex_unlock(cq->mutex);
  }

  return result;
}

// Dynamic queue

// A node of a dynamic queue's doubly-linked list.
typedef struct dllist_node {
  struct dllist_node *prev;
  cmessage msg;
  struct dllist_node *next;
} dllist_node;

// The dynamic queue. It's unbounded, so it has readers to wake but no blocked
// writer and no write_cond. The other fields mean what they mean in struct
// ccircq: Everything is guarded by mutex except the atomic dispatch_refs.
struct cdynmq {
  ccol__mutex mutex;
  ccol__cond_var read_cond;

  size_t readers_waiting;

  size_t msg_count;

  dllist_node *head;
  dllist_node *tail;

  ccol_memmgmt_procs *m_procs;

  bool writing_disabled;

  ccol_sel_waiter *sel_read_waiters_head;
  ccol_sel_waiter *sel_write_waiters_head;

  ccol_sel_waiter *sel_read_rotor;
  ccol_sel_waiter *sel_write_rotor;
  size_t detached_loop_regs;

  _Atomic size_t dispatch_refs;

#if CCOL_FORK_SAFETY_REQUIRED
  size_t registry_idx;
#endif
};

// Appends msg in a new node, taking msg->data from the sender; sendto_dq()
// uses it. Gives ccol_not_enough_memory and leaves msg alone when the node
// can't be allocated. The caller holds the mutex.
static ccol_retval append_msg_to_dq_tail(cdynmq *dq, cmessage *msg) {
  dllist_node *new_elem =
      (dllist_node *)ccol__mem_alloc(dq->m_procs, sizeof(dllist_node));
  if (!new_elem) {
    return ccol_not_enough_memory;
  }

  new_elem->msg.data = msg->data;
  new_elem->msg.size = (msg->data == NULL) ? 0 : msg->size;
  msg->data = NULL;
  new_elem->next = NULL;

  if (!dq->head) {
#ifdef RUNNING_UNIT_TESTS
    ccol__assert(!dq->tail);
#endif
    new_elem->prev = NULL;
    dq->head = new_elem;
    dq->tail = new_elem;
  } else {
#ifdef RUNNING_UNIT_TESTS
    ccol__assert(dq->tail && !dq->tail->next);
#endif
    new_elem->prev = dq->tail;
    dq->tail->next = new_elem;
    dq->tail = new_elem;
  }

  return ccol_success;
}

// Moves the head message into target_buf and frees its node, or gives
// ccol_container_empty; recvfrom_dq() uses it. The caller holds the mutex.
static ccol_retval remove_msg_from_dq_head(cdynmq *dq, cmessage *target_buf) {
  if (!dq->head) {
#ifdef RUNNING_UNIT_TESTS
    ccol__assert(!dq->tail);
#endif
    return ccol_container_empty;
  }

#ifdef RUNNING_UNIT_TESTS
  ccol__assert(!dq->head->prev);
  ccol__assert(dq->tail && !dq->tail->next);
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

  ccol__mem_free(dq->m_procs, node_to_be_freed);
  return ccol_success;
}

// Frees every node of a dynamic queue for its destroy, but not the messages'
// data, which the destroy has already asserted is gone.
static void destroy_dq_dllist(cdynmq *dq) {
  dllist_node *node_to_be_freed = NULL;
  while (dq->head) {
    node_to_be_freed = dq->head;
    dq->head = dq->head->next;
    ccol__mem_free(dq->m_procs, node_to_be_freed);
  }
  dq->tail = NULL;
}

// Creates a dynamic queue, the constructor behind cdynmq_create(): The struct,
// its mutex and a monotonic read_cond, empty and with sends on. Gives the
// queue, or NULL with *err_str set. Registering the mutex for fork safety is
// the last step, so its failure unwinds like any allocation failure.
cdynmq *cdynmq_create_mp(ccol_memmgmt_procs *mmgmt_procs, char **err_str) {
  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  cdynmq *dq = (cdynmq *)ccol__mem_alloc(mmgmt_procs, sizeof(cdynmq));
  if (!dq) {
    if (err_str) {
      *err_str = CCOL__ERR_STR("Failed to allocate memory for cdynmq");
    }
    return NULL;
  }

  if (!ccol__populate_mem_mgmt_procs(dq, mmgmt_procs, err_str)) {
    ccol__mem_free(mmgmt_procs, dq);
    return NULL;
  }

  if (ccol__mutex_init(dq->mutex) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("Failed to initialize dq mutex");
    ccol__mem_free(mmgmt_procs, dq->m_procs);
    ccol__mem_free(mmgmt_procs, dq);
    return NULL;
  }
  if (init_monotonic_cond_var(&dq->read_cond) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("Failed to initialize dq read_cond");
    ccol__mutex_destroy(dq->mutex);
    ccol__mem_free(mmgmt_procs, dq->m_procs);
    ccol__mem_free(mmgmt_procs, dq);
    return NULL;
  }
  dq->msg_count = 0;
  dq->head = NULL;
  dq->tail = NULL;
  dq->readers_waiting = 0;
  dq->writing_disabled = false;
  dq->sel_read_waiters_head = NULL;
  dq->sel_write_waiters_head = NULL;
  dq->sel_read_rotor = NULL;
  dq->sel_write_rotor = NULL;
  dq->detached_loop_regs = 0;
  atomic_init(&dq->dispatch_refs, (size_t)0);

#if CCOL_FORK_SAFETY_REQUIRED
  if (!queue_mutex_registry_add(&dq->mutex, &dq->registry_idx)) {
    if (err_str) {
      *err_str =
          CCOL__ERR_STR("Failed to register cdynmq's mutex for fork safety");
    }
    ccol__mutex_destroy(dq->mutex);
    ccol__cond_var_destroy(dq->read_cond);
    ccol__mem_free(mmgmt_procs, dq->m_procs);
    ccol__mem_free(mmgmt_procs, dq);
    return NULL;
  }
#endif

  if (err_str) {
    *err_str = NULL;
  }

  return dq;
}

// Destroys a dynamic queue for cdynmq_destroy(), with the checks of
// ccircq__destroy(): It asserts on a linked waiter or a detached registration,
// waits for running dispatches, then asserts on queued messages, and frees the
// nodes.
void cdynmq__destroy(cdynmq *dq) {
  if (dq) {
    ccol__mutex_lock(dq->mutex);
    bool has_sel_waiters = (dq->sel_read_waiters_head != NULL) ||
                           (dq->sel_write_waiters_head != NULL) ||
                           dq->detached_loop_regs != 0;
    ccol__mutex_unlock(dq->mutex);
    if (has_sel_waiters) {
      ccol__assert(false);
    }

    queue_wait_for_dispatch_refs(&dq->dispatch_refs, dq);

    if (cdynmq_msg_count(dq) > 0) {
      ccol__assert(false);
    }

#if CCOL_FORK_SAFETY_REQUIRED
    queue_mutex_registry_remove(&dq->mutex, &dq->registry_idx);
#endif

    ccol__mutex_destroy(dq->mutex);
    ccol__cond_var_destroy(dq->read_cond);
    destroy_dq_dllist(dq);

    if (dq->m_procs) {
      ccol_free free_func = dq->m_procs->free;
      free_func(dq->m_procs);
      free_func(dq);
    } else {
      ccol__default_mem_free(dq);
    }
  }
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: The blocked reader count, read under the mutex.
size_t cdynmq__waiting_readers_for_tests(cdynmq *dq) {
  ccol__mutex_lock(dq->mutex);
  size_t n = dq->readers_waiting;
  ccol__mutex_unlock(dq->mutex);
  return n;
}
#endif

// Appends msg, counts it and wakes one reader and one read waiter. Every send
// path of the dynamic queue ends here. Gives ccol_not_enough_memory, leaving
// msg alone, when the node can't be allocated. The caller holds the mutex.
static ccol_retval sendto_dq(cdynmq *dq, cmessage *msg) {
  ccol_retval retval = append_msg_to_dq_tail(dq, msg);

  if (retval == ccol_success) {
    ++dq->msg_count;
    if (dq->readers_waiting) ccol__cond_var_signal(dq->read_cond);
    notify_one_sel_waiter(&dq->sel_read_waiters_head, &dq->sel_read_rotor);
  }

  return retval;
}

// Checks the arguments of every dynamic queue send, like
// verify_circq_send_zc_params().
static bool verify_dynmq_send_zc_params(cdynmq *dq, cmessage *msg) {
  if (!dq || !msg || (msg->size == 0 && msg->data != NULL) ||
      (msg->data == NULL && msg->size != 0)) {
    return false;
  }

  return true;
}

// Appends msg without blocking, the only send of an unbounded queue. Gives
// ccol_not_permitted when sends are off and ccol_container_full at
// ccol_max_elem_count messages.
ccol_retval cdynmq_send(cdynmq *dq, cmessage *msg) {
  if (!verify_dynmq_send_zc_params(dq, msg)) {
    return ccol_invalid_args;
  }

  ccol__mutex_lock(dq->mutex);

  if (dq->writing_disabled) {
    ccol__mutex_unlock(dq->mutex);
    return ccol_not_permitted;
  }

  if (dq->msg_count == ccol_max_elem_count) {
    ccol__mutex_unlock(dq->mutex);
    return ccol_container_full;
  }

  ccol_retval result = sendto_dq(dq, msg);

  ccol__mutex_unlock(dq->mutex);

  return result;
}

// Appends msg whether sends are on or off, so an owner that turned sends off
// can still queue its last control message (see internal/cqueueowner.h).
ccol_retval cdynmq_owner_send(cdynmq *dq, cmessage *msg) {
  if (!verify_dynmq_send_zc_params(dq, msg)) return ccol_invalid_args;
  ccol__mutex_lock(dq->mutex);
  ccol_retval result = dq->msg_count == ccol_max_elem_count
                           ? ccol_container_full
                           : sendto_dq(dq, msg);
  ccol__mutex_unlock(dq->mutex);
  return result;
}

// Takes the head message, counts it and wakes one write waiter. Every receive
// path of the dynamic queue ends here. The caller holds the mutex.
static ccol_retval recvfrom_dq(cdynmq *dq, cmessage *target_buf) {
  ccol_retval retval = remove_msg_from_dq_head(dq, target_buf);

  if (retval == ccol_success) {
    --dq->msg_count;
    notify_one_sel_waiter(&dq->sel_write_waiters_head, &dq->sel_write_rotor);
  }

  return retval;
}

// Checks the arguments of every dynamic queue receive: Neither the queue nor
// the buffer may be NULL.
static bool verify_recvfrom_dq_zc_params(cdynmq *dq, cmessage *target_buf) {
  if (!dq || !target_buf) {
    return false;
  }

  return true;
}

// Receives the head message, blocking on read_cond while the queue is empty,
// for a consumer with nothing else to do.
ccol_retval cdynmq_recv(cdynmq *dq, cmessage *target_buf) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol__mutex_lock(dq->mutex);

  while (dq->msg_count == 0) {
    ++dq->readers_waiting;
    ccol__cond_var_wait(dq->read_cond, dq->mutex);
    --dq->readers_waiting;
  }

  ccol_retval result = recvfrom_dq(dq, target_buf);

  ccol__mutex_unlock(dq->mutex);

  return result;
}

// Receives the head message or gives ccol_container_empty at once, for a
// consumer that must not block.
ccol_retval cdynmq_try_recv(cdynmq *dq, cmessage *target_buf) {
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol_retval result = ccol_container_empty;

  ccol__mutex_lock(dq->mutex);

  if (dq->msg_count > 0) {
    result = recvfrom_dq(dq, target_buf);
  }

  ccol__mutex_unlock(dq->mutex);

  return result;
}

#ifdef RUNNING_UNIT_TESTS
// Test-only flags of the forced wait error of cdynmq_timed_recv.
static _Atomic bool g_dynmq_recv_force_condvar_wait_error = false;
static _Atomic bool g_dynmq_recv_force_condvar_wait_error_also_race = false;

// Test-only: Arms the forced wait error of the next timed receive.
void cdynmq__test_force_next_recv_condvar_wait_error(void) {
  atomic_store(&g_dynmq_recv_force_condvar_wait_error, true);
}

// Test-only: Arms the forced wait error together with an imitated send of a
// sentinel under the same mutex.
void cdynmq__test_force_next_recv_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_dynmq_recv_force_condvar_wait_error, true);
  atomic_store(&g_dynmq_recv_force_condvar_wait_error_also_race, true);
}
#endif

// Receives the head message, waiting at most timeout_us against one deadline,
// for a consumer that also has other work (0 is cdynmq_try_recv()). As in
// ccircq_timed_recv(), a message that arrived wins over a failed wait.
ccol_retval cdynmq_timed_recv(cdynmq *dq, cmessage *target_buf,
                              uint64_t timeout_us) {
  if (timeout_us == 0) return cdynmq_try_recv(dq, target_buf);
  if (!verify_recvfrom_dq_zc_params(dq, target_buf)) {
    return ccol_invalid_args;
  }

  ccol__mutex_lock(dq->mutex);

  if (dq->msg_count == 0) {
    int retval;
    struct timespec abs_time;
    ccol_deadline_after_us(timeout_us, &abs_time);

    while (dq->msg_count == 0) {
      ++dq->readers_waiting;
#ifdef RUNNING_UNIT_TESTS
      if (atomic_load(&g_dynmq_recv_force_condvar_wait_error)) {
        atomic_store(&g_dynmq_recv_force_condvar_wait_error, false);
        retval = EINVAL;
        if (atomic_load(&g_dynmq_recv_force_condvar_wait_error_also_race)) {
          atomic_store(&g_dynmq_recv_force_condvar_wait_error_also_race, false);
          cmessage sentinel = {.data = NULL, .size = 0};
          sendto_dq(dq, &sentinel);
        }
      } else {
        retval = ccol__cond_var_timedwait(dq->read_cond, dq->mutex, abs_time);
      }
#else
      retval = ccol__cond_var_timedwait(dq->read_cond, dq->mutex, abs_time);
#endif
      --dq->readers_waiting;
      if (retval) {
        if (dq->msg_count > 0) break;
        if (retval != ETIMEDOUT) {
          ccol_store_wait_errno(retval);
          ccol__mutex_unlock(dq->mutex);
          return ccol_unexpected_failure;
        }
        ccol__mutex_unlock(dq->mutex);
        return ccol_timed_out;
      }
    }
  }

  ccol_retval result = recvfrom_dq(dq, target_buf);

  ccol__mutex_unlock(dq->mutex);

  return result;
}

// Turns sends off, so an owner can stop new work, and under the mutex wakes
// every write waiter.
ccol_retval cdynmq_disable_sending(cdynmq *dq) {
  if (dq) {
    ccol__mutex_lock(dq->mutex);
    dq->writing_disabled = true;
    notify_all_sel_waiters(dq->sel_write_waiters_head);
    ccol__mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

// Turns sends back on after cdynmq_disable_sending() and wakes every write
// waiter.
ccol_retval cdynmq_enable_sending(cdynmq *dq) {
  if (dq) {
    ccol__mutex_lock(dq->mutex);
    dq->writing_disabled = false;
    notify_all_sel_waiters(dq->sel_write_waiters_head);
    ccol__mutex_unlock(dq->mutex);
    return ccol_success;
  }

  return ccol_invalid_args;
}

// Gives the message count, read under the mutex, or ccol_invalid_size for NULL.
// The destroy uses it, and so can a caller.
size_t cdynmq_msg_count(cdynmq *dq) {
  size_t result = ccol_invalid_size;

  if (dq) {
    ccol__mutex_lock(dq->mutex);
    result = dq->msg_count;
    ccol__mutex_unlock(dq->mutex);
  }

  return result;
}

// Channel

// A channel: Two circular queues and the thread ID of the owner, which routes
// every call.
struct cchan {
  ccol__thread_id owner_tid;
  ccircq *owner_to_workers_cq;
  ccircq *workers_to_owner_cq;
  ccol_memmgmt_procs *m_procs;
};

// Creates a channel for two-way traffic between an owner and its workers: Its
// two circular queues, and the calling thread recorded as the owner. Gives the
// channel, or NULL with *err_str set.
cchan *cchan_create_mp(size_t max_size, ccol_memmgmt_procs *mmgmt_procs,
                       char **err_str) {
  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return NULL;
  }

  cchan *ch = (cchan *)ccol__mem_alloc(mmgmt_procs, sizeof(cchan));
  if (!ch) {
    if (err_str) {
      *err_str = CCOL__ERR_STR("Failed to allocate memory for cchan");
    }
    return NULL;
  }

  if (!ccol__populate_mem_mgmt_procs(ch, mmgmt_procs, err_str)) {
    ccol__mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->owner_to_workers_cq = ccircq_create_mp(max_size, mmgmt_procs, err_str);
  if (!ch->owner_to_workers_cq) {
    ccol__mem_free(mmgmt_procs, ch->m_procs);
    ccol__mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->workers_to_owner_cq = ccircq_create_mp(max_size, mmgmt_procs, err_str);
  if (!ch->workers_to_owner_cq) {
    ccircq_destroy(ch->owner_to_workers_cq);
    ccol__mem_free(mmgmt_procs, ch->m_procs);
    ccol__mem_free(mmgmt_procs, ch);
    return NULL;
  }

  ch->owner_tid = ccol__get_thread_id();

  if (err_str) {
    *err_str = NULL;
  }

  return ch;
}

// Destroys both queues of the channel, then the channel; cchan_destroy() calls
// it.
void cchan__destroy(cchan *ch) {
  if (ch) {
    ccircq_destroy(ch->owner_to_workers_cq);
    ccircq_destroy(ch->workers_to_owner_cq);

    if (ch->m_procs) {
      ccol_free free_func = ch->m_procs->free;
      free_func(ch->m_procs);
      free_func(ch);
    } else {
      ccol__default_mem_free(ch);
    }
  }
}

// Sends on the caller's direction: The owner to owner_to_workers_cq, any other
// thread to workers_to_owner_cq. The thread ID comparison is all the routing.
ccol_retval cchan_send(cchan *ch, cmessage *msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol__get_thread_id() == ch->owner_tid) {
    return ccircq_send(ch->owner_to_workers_cq, msg);
  }

  return ccircq_send(ch->workers_to_owner_cq, msg);
}

// ccircq_try_send() on the caller's direction, for a channel user that must not
// block.
ccol_retval cchan_try_send(cchan *ch, cmessage *msg) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol__get_thread_id() == ch->owner_tid) {
    return ccircq_try_send(ch->owner_to_workers_cq, msg);
  }

  return ccircq_try_send(ch->workers_to_owner_cq, msg);
}

// ccircq_timed_send() on the caller's direction, for a channel user that may
// wait a while.
ccol_retval cchan_timed_send(cchan *ch, cmessage *msg, uint64_t timeout_us) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol__get_thread_id() == ch->owner_tid) {
    return ccircq_timed_send(ch->owner_to_workers_cq, msg, timeout_us);
  }

  return ccircq_timed_send(ch->workers_to_owner_cq, msg, timeout_us);
}

// Receives on the caller's direction: The owner from workers_to_owner_cq, any
// other thread from owner_to_workers_cq.
ccol_retval cchan_recv(cchan *ch, cmessage *target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol__get_thread_id() == ch->owner_tid) {
    return ccircq_recv(ch->workers_to_owner_cq, target_buf);
  }

  return ccircq_recv(ch->owner_to_workers_cq, target_buf);
}

// ccircq_try_recv() on the caller's direction, for a channel user that must not
// block.
ccol_retval cchan_try_recv(cchan *ch, cmessage *target_buf) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol__get_thread_id() == ch->owner_tid) {
    return ccircq_try_recv(ch->workers_to_owner_cq, target_buf);
  }

  return ccircq_try_recv(ch->owner_to_workers_cq, target_buf);
}

// ccircq_timed_recv() on the caller's direction, for a channel user that may
// wait a while.
ccol_retval cchan_timed_recv(cchan *ch, cmessage *target_buf,
                             uint64_t timeout_us) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (ccol__get_thread_id() == ch->owner_tid) {
    return ccircq_timed_recv(ch->workers_to_owner_cq, target_buf, timeout_us);
  }

  return ccircq_timed_recv(ch->owner_to_workers_cq, target_buf, timeout_us);
}

// Turns sends off on direction d, so one side can stop new work. The direction
// is explicit here, since a caller may stop only one side.
ccol_retval cchan_disable_sending(cchan *ch, cchan_direction d) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (d == cchan_owner_to_workers) {
    return ccircq_disable_sending(ch->owner_to_workers_cq);
  } else if (d == cchan_workers_to_owner) {
    return ccircq_disable_sending(ch->workers_to_owner_cq);
  }

  return ccol_invalid_args;
}

// Turns sends back on for direction d.
ccol_retval cchan_enable_sending(cchan *ch, cchan_direction d) {
  if (!ch) {
    return ccol_invalid_args;
  }

  if (d == cchan_owner_to_workers) {
    return ccircq_enable_sending(ch->owner_to_workers_cq);
  } else if (d == cchan_workers_to_owner) {
    return ccircq_enable_sending(ch->workers_to_owner_cq);
  }

  return ccol_invalid_args;
}

// Gives the message count of direction d, for a snapshot, or ccol_invalid_size
// for a bad d.
size_t cchan_msg_count(cchan *ch, cchan_direction d) {
  if (!ch) {
    return ccol_invalid_size;
  }

  if (d == cchan_owner_to_workers) {
    return ccircq_msg_count(ch->owner_to_workers_cq);
  } else if (d == cchan_workers_to_owner) {
    return ccircq_msg_count(ch->workers_to_owner_cq);
  }

  return ccol_invalid_size;
}

// cselect

// Splices node out of a waiter list; every path that takes a waiter off a
// queue uses it. head and rotor must be the pair of that list. A rotor that
// points at node moves to node->next first, since the node can be freed right
// after. The caller holds the queue's mutex and forwards the wake in the same
// critical section.
static void sel_unlink_waiter_locked(ccol_sel_waiter *node,
                                     ccol_sel_waiter **head,
                                     ccol_sel_waiter **rotor) {
  if (*rotor == node) *rotor = node->next;
  if (node->prev)
    node->prev->next = node->next;
  else
    *head = node->next;
  if (node->next) node->next->prev = node->prev;
}

static void queue_sel_locate(cselectable *sel, ccol__mutex **out_mtx,
                             ccol_sel_waiter ***out_head,
                             ccol_sel_waiter ***out_rotor);
static bool queue_sel_is_ready(cselectable *sel);

// Unlinks the waiter node of selectables[i] from its queue, taking the queue's
// mutex, drains the node's eventfd for reuse and marks the slot done
// (sel_mtx = NULL). When the queue is still ready the wake goes on to another
// waiter in the same critical section: A producer wakes exactly one waiter, and
// a waiter that leaves for another reason (deadline, another selectable won,
// an error) would otherwise use that wake up while a message sits in the
// queue. An fd selectable has nothing to unlink.
static void deregister_sel_waiter(size_t i, ccol_sel_waiter *nodes,
                                  cselectable *selectables) {
  if (selectables[i].type == cselectable_type_fd) return;

  ccol__mutex *q_mtx;
  ccol_sel_waiter **head;
  ccol_sel_waiter **rotor;
  queue_sel_locate(&selectables[i], &q_mtx, &head, &rotor);

  ccol__mutex_lock(*q_mtx);
  sel_unlink_waiter_locked(&nodes[i], head, rotor);
  if (queue_sel_is_ready(&selectables[i])) notify_one_sel_waiter(head, rotor);
  ccol__mutex_unlock(*q_mtx);

  if (nodes[i].efd >= 0) {
    eventfd_drain(nodes[i].efd);
  }
  nodes[i].sel_mtx = NULL;
}

// Deregisters every node of a cselect_timed() call whose sel_mtx isn't NULL,
// on every way out of a pass.
static void deregister_all_sel_waiters(size_t n, ccol_sel_waiter *nodes,
                                       cselectable *selectables) {
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].sel_mtx != NULL) {
      deregister_sel_waiter(i, nodes, selectables);
    }
  }
}

// Gives a selectable for the channel queue that the calling thread reads from
// or writes to: For cselect_read the owner watches workers_to_owner_cq and a
// worker owner_to_workers_cq, for cselect_write the other way round. A NULL ch
// gives a selectable with a NULL queue.
cselectable cchan_selectable(cchan *ch, cselect_dir dir) {
  if (!ch) {
    return (cselectable){
        .type = cselectable_type_ccircq, .dir = dir, .cq = NULL};
  }
  bool is_owner = (ccol__get_thread_id() == ch->owner_tid);
  ccircq *cq;
  if (dir == cselect_read) {
    cq = is_owner ? ch->workers_to_owner_cq : ch->owner_to_workers_cq;
  } else {
    cq = is_owner ? ch->owner_to_workers_cq : ch->workers_to_owner_cq;
  }
  return (cselectable){.type = cselectable_type_ccircq, .dir = dir, .cq = cq};
}

// Gives nodes[i] an eventfd registered with epfd under its index, so a queue
// notify wakes the poll wait of a call with fds; once per call. Gives false
// when eventfd or the registration fails. The node must not be linked yet,
// since producers read efd.
static bool sel_ensure_efd(size_t i, ccol_sel_waiter *nodes, int epfd) {
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

// Fills nodes[i] and prepends it to *head, so the queue's producers can wake
// it. cselect_timed() and ceventloop_add_queue() use it. The caller holds the
// queue's mutex.
static void sel_link_waiter(size_t i, ccol_sel_waiter *nodes,
                            ccol__mutex *sel_mtx, ccol__cond_var *sel_cond,
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

// Checks the arguments of cselect_timed() before it allocates anything. Gives
// ccol_success or ccol_invalid_args. n must keep n * sizeof(ccol_sel_waiter)
// from overflowing (the waiter array is one plain malloc) and be at most
// INT_MAX, since the scan carries the match index in an int.
static ccol_retval sel_validate_args(const size_t *ready_index, size_t n,
                                     const cselectable *selectables) {
  if (!ready_index || n == 0 || !selectables) return ccol_invalid_args;
  if (n > SIZE_MAX / sizeof(ccol_sel_waiter)) return ccol_invalid_args;
  if (n > (size_t)INT_MAX) return ccol_invalid_args;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == cselectable_type_ccircq) {
      if (!selectables[i].cq) return ccol_invalid_args;
    } else if (selectables[i].type == cselectable_type_cdynmq) {
      if (!selectables[i].dq) return ccol_invalid_args;
    } else if (selectables[i].type == cselectable_type_fd) {
      if (selectables[i].fd < 0) return ccol_invalid_args;
    } else {
      return ccol_invalid_args;
    }
    if (selectables[i].dir != cselect_read &&
        selectables[i].dir != cselect_write)
      return ccol_invalid_args;
  }
  return ccol_success;
}

// Turns the timeout_us of cselect_timed() into a CLOCK_MONOTONIC deadline and
// gives true, or gives false for UINT64_MAX, which means no deadline.
static bool sel_compute_deadline(uint64_t timeout_us,
                                 struct timespec *deadline) {
  if (timeout_us == UINT64_MAX) return false;
  ccol_deadline_after_us(timeout_us, deadline);
  return true;
}

// The interest bits of one direction of an fd selectable. Registration and
// the mapping of events back to members both use it, so they always agree.
static uint32_t sel_fd_base_events(cselect_dir dir) {
  return (dir == cselect_read)
             ? (uint32_t)(CCOL_POLL_IN | CCOL_POLL_RDHUP | CCOL_POLL_ERR |
                          CCOL_POLL_HUP)
             : (uint32_t)(CCOL_POLL_OUT | CCOL_POLL_ERR | CCOL_POLL_HUP);
}

// Tag bit in the event data of an fd group's registration. The other bits hold
// the position of the group's first member in the sorted pairs, so an event
// leads to its group without a search. A queue eventfd's data is an index below
// n, and the position stays below INT_MAX, keeping bit 62 (reserved by the
// kqueue translation) clear.
#define SEL_FD_TAG ((uint64_t)1 << 63)

// One fd selectable and its index in selectables[]. Sorted by fd, the
// selectables of one fd form a contiguous run.
typedef struct {
  int fd;
  size_t idx;
} sel_fd_pair;

// What a cselect_timed() call with fd selectables keeps. pairs and pfds hold
// the fd selectables in the same order (Phase 1 polls them without any kernel
// state), and fd_ready one flag per selectable; the three share one block.
// Only a call that blocks builds the poll set, which sorts pairs and pfds by fd
// then index, and evs, which has room for every filter the set can report at
// once.
typedef struct {
  sel_fd_pair *pairs;
  struct pollfd *pfds;
  size_t n_pairs;
  ccol_poll_event *evs;
  int ev_cap;
  bool *fd_ready;
  bool has_queues;
} sel_fd_state;

// Frees what sel_fd_state_init() and sel_setup_epoll() allocated: The block of
// pairs, pfds and fd_ready, and the event buffer.
static void sel_fd_state_free(sel_fd_state *fs) {
  free(fs->pairs);
  free(fs->evs);
}

// The calling thread's rotation state for the scan of cselect_timed(). Each
// call adds the 32-bit golden ratio (a Weyl sequence), so starts spread evenly
// over [0, n) even when a thread alternates between sets. Per thread, it writes
// no shared cache line.
static __thread uint32_t sel_rotation;

// Gives the next start index in [0, n) for the scan of cselect_timed(), so
// ready selectables share the calls; 0 for a single selectable. Advances the
// thread's sel_rotation.
static size_t sel_rotation_start(size_t n) {
  if (n < 2) return 0;
  sel_rotation += 0x9E3779B9u;
  return (size_t)(((uint64_t)sel_rotation * (uint64_t)n) >> 32);
}

// Turns the events of the last poll wait into fs->fd_ready for
// sel_wait_epoll(): The flag of every fd selectable whose direction an event
// satisfies is set and the rest cleared. Each event is refined with the
// combined mask of its fd group; queue eventfd events and events that refine to
// nothing set no flag. Gives true when any flag is set.
static bool sel_mark_fd_events(sel_fd_state *fs, size_t n,
                               cselectable *selectables, int n_ev) {
  memset(fs->fd_ready, 0, n * sizeof(bool));
  bool any = false;
  for (int e = 0; e < n_ev; e++) {
    ccol_poll_event *ev = &fs->evs[e];
    if (!(ev->data.u64 & SEL_FD_TAG)) continue;
    size_t run = (size_t)(ev->data.u64 & ~SEL_FD_TAG);
    int fd = fs->pairs[run].fd;
    size_t end = run;
    uint32_t combined = 0;
    while (end < fs->n_pairs && fs->pairs[end].fd == fd) {
      combined |= sel_fd_base_events(selectables[fs->pairs[end].idx].dir);
      end++;
    }
    ccol_poll_refine(ev, fd, combined, NULL);
    for (size_t j = run; j < end; j++) {
      size_t idx = fs->pairs[j].idx;
      if (sel_fd_base_events(selectables[idx].dir) & ev->events) {
        fs->fd_ready[idx] = true;
        any = true;
      }
    }
  }
  return any;
}

// qsort() order of the pairs, by fd and then by index, which groups the
// selectables of one fd for sel_setup_epoll().
static int sel_fd_pair_cmp(const void *a, const void *b) {
  const sel_fd_pair *pa = (const sel_fd_pair *)a;
  const sel_fd_pair *pb = (const sel_fd_pair *)b;
  if (pa->fd != pb->fd) return (pa->fd < pb->fd) ? -1 : 1;
  if (pa->idx != pb->idx) return (pa->idx < pb->idx) ? -1 : 1;
  return 0;
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: The number of the cselect allocation that fails, 0 when none.
static _Atomic uint32_t g_sel_fail_alloc_at = 0;

// Test-only: How many poll sets cselect_timed() created.
static _Atomic uint64_t g_sel_poll_sets_created = 0;

// Test-only: Arms the failing allocation.
void cselect__test_fail_alloc(uint32_t index) {
  atomic_store(&g_sel_fail_alloc_at, index);
}

// Test-only: Reads the count of poll sets.
uint64_t cselect__test_poll_sets_created(void) {
  return atomic_load(&g_sel_poll_sets_created);
}
#endif

// malloc() for every allocation of cselect_timed(), so a test can make any one
// of them fail.
static void *sel_malloc(size_t size) {
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

// Fills *fs with the fd selectables in array order and allocates the block of
// pairs, pfds and fd_ready; cselect_timed() calls it for a set with fds. Free
// it with sel_fd_state_free() whatever the result. Gives ccol_success or
// ccol_not_enough_memory.
static ccol_retval sel_fd_state_init(size_t n, cselectable *selectables,
                                     sel_fd_state *fs) {
  fs->pairs = NULL;
  fs->pfds = NULL;
  fs->evs = NULL;
  fs->ev_cap = 0;
  fs->fd_ready = NULL;
  fs->n_pairs = 0;
  size_t n_fd = 0;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == cselectable_type_fd) n_fd++;
  }
  fs->has_queues = n_fd < n;

  _Static_assert(_Alignof(sel_fd_pair) % _Alignof(struct pollfd) == 0,
                 "the pollfd array must be aligned after the pairs");
  if (n_fd > SIZE_MAX / (sizeof(sel_fd_pair) + sizeof(struct pollfd)) ||
      n > SIZE_MAX - n_fd * (sizeof(sel_fd_pair) + sizeof(struct pollfd)))
    return ccol_not_enough_memory;
  size_t pfds_off = n_fd * sizeof(sel_fd_pair);
  size_t ready_off = pfds_off + n_fd * sizeof(struct pollfd);
  unsigned char *block = sel_malloc(ready_off + n * sizeof(bool));
  if (!block) return ccol_not_enough_memory;
  fs->pairs = (sel_fd_pair *)(void *)block;
  fs->pfds = (struct pollfd *)(void *)(block + pfds_off);
  fs->fd_ready = (bool *)(void *)(block + ready_off);

  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type != cselectable_type_fd) continue;
    fs->pairs[k].fd = selectables[i].fd;
    fs->pairs[k].idx = i;
    fs->pfds[k].fd = selectables[i].fd;
    fs->pfds[k].events =
        (selectables[i].dir == cselect_read) ? POLLIN : POLLOUT;
    fs->pfds[k].revents = 0;
    k++;
  }
  fs->n_pairs = k;
  return ccol_success;
}

// Asks poll(2), without waiting, which fd selectables are ready and records it
// in fs->fd_ready. Phase 1 of cselect_timed() uses it, so a call answered at
// once needs no poll set. POLLIN counts for a read selectable, POLLOUT for a
// write one, and POLLERR or POLLHUP for both. A ready fd that is a regular file
// or a directory fails with EPERM (poll calls such a file always ready, epoll
// refuses it), and an fd that isn't open with EBADF. Gives 0, or -1 with errno
// set.
static int sel_poll_fds(sel_fd_state *fs, size_t n, cselectable *selectables) {
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
    short want = (selectables[idx].dir == cselect_read) ? POLLIN : POLLOUT;
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

// Builds what a call needs to block: The event buffer, a poll set with one
// level-triggered registration per fd (the selectables of one fd are combined
// into one registration with the OR of their masks, since a second ADD of the
// same fd fails), and an eventfd for each queue waiter. Grouping sorts the
// pairs, O(n log n), and rebuilds pfds in the same order. Every node must be
// unlinked here. Gives ccol_success, ccol_not_enough_memory for the buffer, or
// ccol_unexpected_failure; *epfd is then -1 or a set the caller closes.
static ccol_retval sel_setup_epoll(size_t n, cselectable *selectables,
                                   ccol_sel_waiter *nodes, sel_fd_state *fs,
                                   int *epfd) {
  size_t ev_cap = n + fs->n_pairs;
  if (ev_cap > (size_t)INT_MAX) ev_cap = (size_t)INT_MAX;
  if (ev_cap > SIZE_MAX / sizeof(ccol_poll_event))
    return ccol_not_enough_memory;
  fs->evs = sel_malloc(ev_cap * sizeof(ccol_poll_event));
  if (!fs->evs) return ccol_not_enough_memory;
  fs->ev_cap = (int)ev_cap;

  *epfd = ccol_poll_create();
  if (*epfd < 0) return ccol_unexpected_failure;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_sel_poll_sets_created, (uint64_t)1);
#endif

  size_t n_pairs = fs->n_pairs;
  qsort(fs->pairs, n_pairs, sizeof(sel_fd_pair), sel_fd_pair_cmp);
  for (size_t k = 0; k < n_pairs; k++) {
    size_t idx = fs->pairs[k].idx;
    fs->pfds[k].fd = fs->pairs[k].fd;
    fs->pfds[k].events =
        (selectables[idx].dir == cselect_read) ? POLLIN : POLLOUT;
    fs->pfds[k].revents = 0;
  }

  for (size_t i = 0; i < n_pairs;) {
    int fd = fs->pairs[i].fd;
    uint32_t combined = 0;
    size_t j = i;
    while (j < n_pairs && fs->pairs[j].fd == fd) {
      combined |= sel_fd_base_events(selectables[fs->pairs[j].idx].dir);
      j++;
    }

    ccol_poll_event ev;
    ev.data.u64 = SEL_FD_TAG | (uint64_t)i;
    ev.events = combined;
    if (ccol_poll_ctl(*epfd, CCOL_POLL_CTL_ADD, fd, &ev, NULL) < 0)
      return ccol_unexpected_failure;
    i = j;
  }

  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type != cselectable_type_fd &&
        !sel_ensure_efd(i, nodes, *epfd))
      return ccol_unexpected_failure;
  }
  return ccol_success;
}

// Phase 1: Scans the selectables in rotation order from start and stops at the
// first ready one. With fds it first polls them once without waiting, and a
// ready fd competes like a ready queue. A queue is only peeked at, under its
// mutex: A ready one also forwards a wake to its next waiter (this caller
// consumes nothing and is about to leave), and one that isn't ready gets its
// node linked. Write readiness is room or sends off. Gives the found index, -1
// when nothing is ready and every queue node is linked, or -2 when the poll
// fails, before any node is linked. fs is NULL without fds.
static int sel_phase1_scan_register(size_t n, cselectable *selectables,
                                    ccol_sel_waiter *nodes, size_t start,
                                    sel_fd_state *fs, ccol__mutex *sel_mtx,
                                    ccol__cond_var *sel_cond, bool *ready) {
  bool has_fd_sels = fs != NULL;
  if (has_fd_sels && sel_poll_fds(fs, n, selectables) != 0) return -2;
  int found = -1;
  size_t i = start;
  for (size_t left = n; left != 0 && found < 0;
       left--, i = (i + 1 == n) ? 0 : i + 1) {
    if (selectables[i].type == cselectable_type_fd) {
      if (has_fd_sels && fs->fd_ready[i]) found = (int)i;
      continue;
    }

    if (selectables[i].type == cselectable_type_ccircq) {
      ccircq *cq = selectables[i].cq;
      ccol__mutex_lock(cq->mutex);

      if (selectables[i].dir == cselect_read) {
        if (cq->msg_count > 0) {
          notify_one_sel_waiter(&cq->sel_read_waiters_head,
                                &cq->sel_read_rotor);
          ccol__mutex_unlock(cq->mutex);
          found = (int)i;
        } else {
          sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                          &cq->sel_read_waiters_head);
          ccol__mutex_unlock(cq->mutex);
        }
      } else {
        if (cq->msg_count < cq->max_size || cq->writing_disabled) {
          notify_one_sel_waiter(&cq->sel_write_waiters_head,
                                &cq->sel_write_rotor);
          ccol__mutex_unlock(cq->mutex);
          found = (int)i;
        } else {
          sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                          &cq->sel_write_waiters_head);
          ccol__mutex_unlock(cq->mutex);
        }
      }
    } else {
      cdynmq *dq = selectables[i].dq;
      ccol__mutex_lock(dq->mutex);

      if (selectables[i].dir == cselect_read) {
        if (dq->msg_count > 0) {
          notify_one_sel_waiter(&dq->sel_read_waiters_head,
                                &dq->sel_read_rotor);
          ccol__mutex_unlock(dq->mutex);
          found = (int)i;
        } else {
          sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                          &dq->sel_read_waiters_head);
          ccol__mutex_unlock(dq->mutex);
        }
      } else {
        if (dq->writing_disabled || dq->msg_count < ccol_max_elem_count) {
          notify_one_sel_waiter(&dq->sel_write_waiters_head,
                                &dq->sel_write_rotor);
          ccol__mutex_unlock(dq->mutex);
          found = (int)i;
        } else {
          sel_link_waiter(i, nodes, sel_mtx, sel_cond, ready,
                          &dq->sel_write_waiters_head);
          ccol__mutex_unlock(dq->mutex);
        }
      }
    }
  }
  return found;
}

// How a condition-variable wait of cselect_timed() ended.
typedef enum {
  SEL_CONDVAR_READY,
  SEL_CONDVAR_TIMED_OUT,
  SEL_CONDVAR_FAILURE,
} sel_condvar_outcome;

#ifdef RUNNING_UNIT_TESTS
// Test-only: Makes the next timed condition wait of sel_wait_condvar() give
// EINVAL without waiting (no correct deadline can cause that).
static _Atomic bool g_sel_force_condvar_wait_error = false;

// Test-only: With the flag above, also sets *ready under the same mutex, as a
// producer whose notify lands just before the error.
static _Atomic bool g_sel_force_condvar_wait_error_also_marks_ready = false;

// Test-only: How many of the next capped waits (INT_MAX ms) of sel_wait_epoll
// return 0 at once.
static _Atomic uint32_t g_sel_short_capped_waits = 0;

// Test-only: Sets and reads the count of capped waits that end at once.
void cselect__test_short_next_capped_waits(uint32_t count) {
  atomic_store(&g_sel_short_capped_waits, count);
}

uint32_t cselect__test_short_capped_waits_left(void) {
  return atomic_load(&g_sel_short_capped_waits);
}

// Test-only: ccol_poll_wait() that ends an armed capped wait at once.
static int sel_poll_wait_for_tests(int epfd, ccol_poll_event *evs, int cap,
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

// Test-only: Arm the forced condition wait error, with or without the ready
// flag.
void cselect__test_force_next_condvar_wait_error(void) {
  atomic_store(&g_sel_force_condvar_wait_error, true);
}

void cselect__test_force_next_condvar_wait_error_racing_ready(void) {
  atomic_store(&g_sel_force_condvar_wait_error, true);
  atomic_store(&g_sel_force_condvar_wait_error_also_marks_ready, true);
}

// Test-only: The armed delay between a timed-out condition wait and the
// deregister step, and how many times a thread entered it.
static _Atomic uint32_t g_sel_timed_out_deregister_delay_us = 0;
static _Atomic uint64_t g_sel_timed_out_deregister_delays = 0;

// Test-only: Arms the delay and reads the entry count.
void cselect__test_delay_next_timed_out_deregister_us(uint32_t us) {
  atomic_store(&g_sel_timed_out_deregister_delay_us, us);
}

uint64_t cselect__test_timed_out_deregister_delay_count(void) {
  return atomic_load(&g_sel_timed_out_deregister_delays);
}

// Test-only: Takes the armed delay once and counts it.
static void sel_timed_out_deregister_test_delay(void) {
  uint32_t us = atomic_exchange(&g_sel_timed_out_deregister_delay_us, 0u);
  if (us == 0) return;
  atomic_fetch_add(&g_sel_timed_out_deregister_delays, (uint64_t)1);
  struct timespec ts = {.tv_sec = us / 1000000,
                        .tv_nsec = (long)(us % 1000000) * 1000L};
  nanosleep(&ts, NULL);
}
#endif

// Phase 2 of cselect_timed() without fds: Waits on sel_cond until a producer
// sets *ready or the deadline passes, and clears *ready before it returns. A
// wait with no deadline always gives SEL_CONDVAR_READY. With a deadline,
// ETIMEDOUT gives SEL_CONDVAR_TIMED_OUT and any other error
// SEL_CONDVAR_FAILURE, so the loop can't spin on an error; but a *ready that is
// set when either is seen wins, since the producer may have finished just
// before. Locks *sel_mtx.
static sel_condvar_outcome sel_wait_condvar(ccol__mutex *sel_mtx,
                                            ccol__cond_var *sel_cond,
                                            bool *ready, bool has_deadline,
                                            const struct timespec *deadline) {
  ccol__mutex_lock(*sel_mtx);
  if (has_deadline) {
    sel_condvar_outcome outcome = SEL_CONDVAR_READY;
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
        wait_ret = ccol__cond_var_timedwait(*sel_cond, *sel_mtx, *deadline);
      }
#else
      wait_ret = ccol__cond_var_timedwait(*sel_cond, *sel_mtx, *deadline);
#endif
      if (wait_ret == ETIMEDOUT) {
        if (!*ready) outcome = SEL_CONDVAR_TIMED_OUT;
        break;
      }
      if (wait_ret != 0) {
        if (!*ready) outcome = SEL_CONDVAR_FAILURE;
        break;
      }
    }
    *ready = false;
    ccol__mutex_unlock(*sel_mtx);
    return outcome;
  }
  while (!*ready) ccol__cond_var_wait(*sel_cond, *sel_mtx);
  *ready = false;
  ccol__mutex_unlock(*sel_mtx);
  return SEL_CONDVAR_READY;
}

// How an epoll wait of cselect_timed() ended.
typedef enum {
  SEL_EPOLL_CONTINUE,
  SEL_EPOLL_BREAK,
  SEL_EPOLL_FAILURE,
} sel_epoll_outcome;

// Phase 2 with fds: Waits on the poll set until something is ready or the
// deadline passes. The remaining time is recomputed before every wait, rounded
// up and capped at INT_MAX ms; a wait that returns nothing before the deadline
// waits again, and after it the call deregisters and gives ccol_timed_out. When
// only queue eventfds fired (or fd events that refine to nothing) it gives
// SEL_EPOLL_CONTINUE for Phase 3. Otherwise the ready fd first in rotation
// order from start wins: It deregisters, sets *ready_index and *out_retval and
// gives SEL_EPOLL_BREAK. A system error gives SEL_EPOLL_FAILURE with the nodes
// still linked.
static sel_epoll_outcome sel_wait_epoll(
    int epfd, bool has_deadline, const struct timespec *deadline, size_t n,
    cselectable *selectables, ccol_sel_waiter *nodes, sel_fd_state *fs,
    size_t start, size_t *ready_index, ccol_retval *out_retval) {
  int n_ready;
  int epoll_to;
  for (;;) {
    if (has_deadline) {
      epoll_to = ccol_deadline_remaining_ms_ceil(deadline);
    } else {
      epoll_to = -1;
    }
#ifdef RUNNING_UNIT_TESTS
    n_ready = sel_poll_wait_for_tests(epfd, fs->evs, fs->ev_cap, epoll_to);
#else
    n_ready = ccol_poll_wait(epfd, fs->evs, fs->ev_cap, epoll_to);
#endif
    if (n_ready < 0 && errno == EINTR) continue;
    if (n_ready == 0 && epoll_to != 0 &&
        (!has_deadline || ccol_deadline_remaining_ms_ceil(deadline) > 0))
      continue;
    break;
  }

  if (n_ready == 0) {
    deregister_all_sel_waiters(n, nodes, selectables);
    *out_retval = ccol_timed_out;
    return SEL_EPOLL_BREAK;
  }
  if (n_ready < 0) return SEL_EPOLL_FAILURE;

  if (!sel_mark_fd_events(fs, n, selectables, n_ready))
    return SEL_EPOLL_CONTINUE;
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

  *out_retval = ccol_success;
  *ready_index = match;
  return SEL_EPOLL_BREAK;
}

// Waits until one of n selectables is ready or timeout_us passes (UINT64_MAX:
// no limit) and sets *ready_index; the caller then does the receive or send.
// Each iteration has three phases. Phase 1 scans in rotation order and links a
// waiter node into every queue that isn't ready (sel_phase1_scan_register). The
// first time a call with fds must block, it unlinks, builds the poll set and
// the queue eventfds and scans again, so no wake is lost. Phase 2 waits: On
// sel_cond without fds (a producer sets *ready under sel_mtx before it
// signals), on the poll set with fds, where queue notifies arrive as eventfd
// writes. Phase 3 unlinks every node, forwarding a wake from a queue that is
// still ready, drains the eventfds, clears *ready and goes back to Phase 1. The
// lock order is always a queue's mutex, then sel_mtx, and a producer can't
// unlink a node it's walking, since both need the queue's mutex. The poll set
// and eventfds are made at most once per call and closed at the end.
ccol_retval cselect_timed(size_t *ready_index, size_t n,
                          cselectable *selectables, uint64_t timeout_us) {
  ccol_retval v = sel_validate_args(ready_index, n, selectables);
  if (v != ccol_success) return v;

  bool has_fd_sels = false;
  for (size_t i = 0; i < n; i++) {
    if (selectables[i].type == cselectable_type_fd) {
      has_fd_sels = true;
      break;
    }
  }

  ccol_sel_waiter *nodes = sel_malloc(n * sizeof(ccol_sel_waiter));
  if (!nodes) return ccol_not_enough_memory;
  for (size_t i = 0; i < n; i++) {
    nodes[i].sel_mtx = NULL;
    nodes[i].efd = -1;
  }

  ccol__mutex sel_mtx;
  ccol__cond_var sel_cond;
  bool ready = false;
  if (ccol__mutex_init(sel_mtx) != 0) {
    free(nodes);
    return ccol_unexpected_failure;
  }
  if (init_monotonic_cond_var(&sel_cond) != 0) {
    ccol__mutex_destroy(sel_mtx);
    free(nodes);
    return ccol_unexpected_failure;
  }

  bool has_deadline;
  struct timespec deadline = {0, 0};
  has_deadline = sel_compute_deadline(timeout_us, &deadline);

  int epfd = -1;
  sel_fd_state fd_state;
  sel_fd_state *fs = NULL;
  if (has_fd_sels) {
    fs = &fd_state;
    if (sel_fd_state_init(n, selectables, fs) != ccol_success) {
      sel_fd_state_free(fs);
      free(nodes);
      ccol__mutex_destroy(sel_mtx);
      ccol__cond_var_destroy(sel_cond);
      return ccol_not_enough_memory;
    }
  }
  ccol_retval failure = ccol_unexpected_failure;

  size_t start = sel_rotation_start(n);

  ccol_retval retval = ccol_success;

  for (;;) {
    int found = sel_phase1_scan_register(n, selectables, nodes, start, fs,
                                         &sel_mtx, &sel_cond, &ready);
    if (found == -2) goto cleanup_failure;
    if (found >= 0) {
      deregister_all_sel_waiters(n, nodes, selectables);
      *ready_index = (size_t)found;
      retval = ccol_success;
      break;
    }

    if (has_fd_sels && epfd < 0) {
      deregister_all_sel_waiters(n, nodes, selectables);
      if (has_deadline && ccol_deadline_remaining_ms_ceil(&deadline) == 0) {
        retval = ccol_timed_out;
        break;
      }
      ccol__mutex_lock(sel_mtx);
      ready = false;
      ccol__mutex_unlock(sel_mtx);
      failure = sel_setup_epoll(n, selectables, nodes, fs, &epfd);
      if (failure != ccol_success) goto cleanup_failure;
      failure = ccol_unexpected_failure;
      continue;
    }

    if (!has_fd_sels) {
      sel_condvar_outcome outcome = sel_wait_condvar(
          &sel_mtx, &sel_cond, &ready, has_deadline, &deadline);
      if (outcome == SEL_CONDVAR_TIMED_OUT) {
#ifdef RUNNING_UNIT_TESTS
        sel_timed_out_deregister_test_delay();
#endif
        deregister_all_sel_waiters(n, nodes, selectables);
        retval = ccol_timed_out;
        break;
      }
      if (outcome == SEL_CONDVAR_FAILURE) goto cleanup_failure;
    } else {
      sel_epoll_outcome outcome =
          sel_wait_epoll(epfd, has_deadline, &deadline, n, selectables, nodes,
                         fs, start, ready_index, &retval);
      if (outcome == SEL_EPOLL_BREAK) break;
      if (outcome == SEL_EPOLL_FAILURE) goto cleanup_failure;
    }

    deregister_all_sel_waiters(n, nodes, selectables);
    ccol__mutex_lock(sel_mtx);
    ready = false;
    ccol__mutex_unlock(sel_mtx);
  }

  for (size_t i = 0; i < n; i++) {
    if (nodes[i].efd >= 0) close(nodes[i].efd);
  }
  if (epfd >= 0) close(epfd);
  if (fs) sel_fd_state_free(fs);
  ccol__mutex_destroy(sel_mtx);
  ccol__cond_var_destroy(sel_cond);
  free(nodes);
  return retval;

cleanup_failure:
  deregister_all_sel_waiters(n, nodes, selectables);
  for (size_t i = 0; i < n; i++) {
    if (nodes[i].efd >= 0) close(nodes[i].efd);
  }
  if (epfd >= 0) close(epfd);
  if (fs) sel_fd_state_free(fs);
  ccol__mutex_destroy(sel_mtx);
  ccol__cond_var_destroy(sel_cond);
  free(nodes);
  return failure;
}

// cselect_timed() with no time limit.
ccol_retval cselect(size_t *ready_index, size_t n, cselectable *selectables) {
  return cselect_timed(ready_index, n, selectables, UINT64_MAX);
}

// ceventloop

typedef struct event_entry event_entry;

// A registration. The public ceventloop_reg is an opaque handle resolved
// through the loop's reg_slots table, never a pointer to this struct.
typedef struct ceventloop_reg_s ceventloop_reg_s;
struct ceventloop_reg_s {
  cselectable sel;
  ceventloop_handlers handlers;
  void *arg;
  // 1 while registered, plus one for each callback that runs.
  _Atomic int refcount;
  _Atomic bool removed;

  // Set between ceventloop_pause and ceventloop_resume: The registration keeps
  // its slot, count and generation but leaves the fd's interest mask, which
  // every re-arm recomputes from live state. Always false for a queue
  // registration.
  _Atomic bool paused;

  // Set when this fd registration got an event no handler of it can take
  // (usually a hang-up, which level-triggered epoll repeats), so it leaves the
  // interest mask like a paused one. Collection sets it, modify and resume
  // clear it, always under the stripe lock.
  bool muted;

  event_entry *owning_entry;

  // The stripe of the entry, the generation and the public handle, all written
  // once before the registration is wired. They're read from here and never
  // through owning_entry, because a stale reg can outlive its entry: The two
  // have separate deferred-free lists. A dispatch hands self_handle to every
  // callback, so a callback can act on its registration before the add returns.
  size_t stripe_idx;

  uint64_t generation;

  ceventloop_reg self_handle;

  // Index into loop->reg_slots, which ceventloop_remove invalidates.
  uint32_t self_slot_idx;

  // Pins taken by ceventloop_reg_resolve and dropped by the unpin. The reclaim
  // never frees a reg while it isn't zero, which covers a public call that is
  // in the middle of using the reg.
  _Atomic size_t pending_resolve_count;

  // For a queue registration: The persistent bridge eventfd, -1 for an fd. A
  // queue wake is recorded under wait_mtx in wait_ready, which is true exactly
  // while a wake is pending on bridge_efd; queue_reg_consume_wake clears both
  // right before a dispatch, so one write covers every send up to it. Nothing
  // waits on wait_cond. waiter_detached (under the queue's mutex) is set once
  // the loop's shutdown unlinked waiter_node, so it isn't unlinked twice.
  int bridge_efd;

  ccol__mutex wait_mtx;
  ccol__cond_var wait_cond;
  bool wait_ready;
  ccol_sel_waiter waiter_node;
  bool waiter_detached;

  // Links of the stripe's list of queue registrations, which the destroy walks
  // to unlink every waiter node; an fd registration is reached through the fd
  // table instead.
  ceventloop_reg_s *loop_list_prev;
  ceventloop_reg_s *loop_list_next;

  // Link in loop->pending_reg_frees once the refcount reached 0.
  ceventloop_reg_s *pending_free_next;
};

// What the event data of every poll registration of the loop points at. Up to
// two fd registrations (read and write) share one entry, because the kernel
// keys its interest list by fd alone; a queue registration has its own bridge
// eventfd and entry.
struct event_entry {
  bool is_fd;
  int fd;

  // The stripe whose lock guards as.* and whose index holds the entry. Set
  // before the entry is published and never written again, so it's read without
  // a lock; never derive it from as.reg, which the remove sets to NULL while a
  // stale batch may still point at the entry.
  size_t stripe_idx;

  // Held across the entry's whole dispatch (collection and callback), so one
  // registration never runs concurrently with itself, and the read and write
  // registrations of an fd never run at the same time. It's taken before a
  // stripe lock and released before any other stripe lock, so it adds no cycle.
  ccol__mutex dispatch_lock;

  // The identity token of ceventloop_reg_generation, made once per new entry.
  uint64_t generation;

  // poller_batch_gen when the entry was deferred for freeing, and the flag set
  // at that moment; the re-arm skips a removed entry, whose fd may be closed or
  // reused.
  uint64_t defer_gen;

  _Atomic bool removed;

  // Dispatch jobs that reference the entry (only with a dispatch pool). The
  // reclaim doesn't free a deferred entry while it isn't zero, since a job can
  // run long after the poller has moved on.
  _Atomic size_t refcount;

  union {
    struct {
      ceventloop_reg_s *read_reg;
      ceventloop_reg_s *write_reg;
    } fd;
    ceventloop_reg_s *reg;
  } as;

  // Whether the fd is in the poll set right now. ERR and HUP are reported
  // whatever the mask, so an fd whose live directions are all paused (or muted)
  // is taken out with DEL, or it would be reported over and over; it's added
  // back with ADD. armed_in is the IN bit of the last ADD or MOD, so a MOD that
  // flips it carries CCOL_POLL_READ_MODE_CHANGE, and poll_info is the cache of
  // cpoll.h. All three are kept under the stripe lock and are unused for a
  // queue entry.
  bool epoll_added;
  bool armed_in;
  ccol_poll_fdinfo poll_info;

  // Link in loop->pending_entry_frees once the entry is retired.
  event_entry *pending_free_next;
};

// One lock stripe of a loop's registry: A mutex, the fd index of its real fds
// and the list of its queue registrations. An fd (or a queue's bridge eventfd)
// belongs to one stripe for life, so no operation holds two stripe locks.
typedef struct ceventloop_stripe {
  ccol__mutex lock;
  chmap fd_index;
  ceventloop_reg_s *queue_regs_head;
} ceventloop_stripe;

// One slot of the process-wide loop table. A loop handle is the slot index in
// the top 32 bits and the generation in the bottom 32. The generation rises on
// every acquire, so a stale handle never names a later loop, and the destroy
// can report a double destroy instead of freeing twice.
typedef struct {
  struct ceventloop__s *ptr;
  uint32_t generation;
  bool in_use;
} ceventloop_slot;

// The process-wide table of loop slots. The lock is a read-write lock, since a
// resolve only reads and runs on every pause, resume, modify and remove, while
// only create and destroy write. slots only grows, so an index is permanent;
// free_indices is a LIFO of reusable indices. lost_indices counts released
// slots that the free list couldn't take for lack of memory; an acquire with an
// empty free list recovers one by a scan. release_deferred is set when the exit
// destructor found a live loop, and the destroy of the last loop releases the
// table.
static struct {
  ccol__rw_lock rwlock;
  ccol__once_flag once;
  cvec slots;
  cvec free_indices;
  size_t lost_indices;
  bool release_deferred;
} ceventloop_slot_table = {0};

static void release_event_loop_slot_table_if_deferred_locked(void);

// Gives how many slots the table holds, 0 once the exit destructor released
// it, so a resolve finds nothing and a create fails cleanly. The caller holds
// either side of the table lock.
static inline size_t event_loop_slot_count_locked(void) {
  return ceventloop_slot_table.slots
             ? cvector_elem_count(ceventloop_slot_table.slots)
             : 0;
}

#if CCOL_FORK_SAFETY_REQUIRED
static void cthreadcomm_atfork_prepare(void);
static void cthreadcomm_atfork_release(void);
static void cthreadcomm_atfork_child_release(void);
#endif

// Creates the table's lock and vectors on first use; the resolve, the acquire
// and the fork registration need them. A failure is fatal.
static void ceventloop_slot_table_init_globals(void) {
  if (ccol__rw_lock_init(ceventloop_slot_table.rwlock) != 0)
    ccol__fatal_err("ceventloop slot table: failed to initialize rwlock");
  ceventloop_slot_table.slots = cvector_create(sizeof(ceventloop_slot), NULL);
  if (!ceventloop_slot_table.slots)
    ccol__fatal_err("ceventloop slot table: failed to allocate slots vector");
  ceventloop_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!ceventloop_slot_table.free_indices)
    ccol__fatal_err(
        "ceventloop slot table: failed to allocate free-index vector");
}

// Registers the one merged set of fork handlers that covers the loop table and
// the queue mutex registry; two separate sets would deadlock. It first readies
// both structures, since a process may use only queues and the handlers walk
// both. Called once through g_cthreadcomm_atfork_once.
static void cthreadcomm_register_atfork_once(void) {
#if CCOL_FORK_SAFETY_REQUIRED
  ccol__call_once(queue_mutex_registry.once, queue_mutex_registry_init_globals);
  ccol__call_once(ceventloop_slot_table.once,
                  ceventloop_slot_table_init_globals);
  ccol__at_fork(cthreadcomm_atfork_prepare, cthreadcomm_atfork_release,
                cthreadcomm_atfork_child_release);
#endif
}

#if defined(CCOL__ATFORK_AT_LOAD)
// Registers the fork handlers at load time where CCOL__ATFORK_AT_LOAD asks for
// that (see common.h).
__attribute__((constructor)) static void cthreadcomm_atfork_at_load(void) {
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
}
#endif

// Registers this module's fork handlers now, so that they run after the
// caller's prepare handler (prepare handlers run in reverse order). chttpserver
// calls it before it registers its own, which rules out a lock order inversion
// between the two sets. Declared in internal/catfork.h.
void cthreadcomm__ensure_atfork_registered_before_caller(void) {
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
}

// A loop. The registry lives in the stripes, each with its own lock; the other
// fields are described per group below.
struct ceventloop__s {
  // The poll set the poller waits on, and the eventfd a shutdown writes once
  // and nothing ever drains, so every wait sees it.
  int epfd;
  int shutdown_efd;

  // A wake descriptor that pings the poller between batches. ceventloop_remove
  // pings it for a registration with on_removed, so the notice comes promptly
  // on an idle loop. It's only for latency: The bounded retry timeout of the
  // poller is what guarantees a pending reclaim runs.
  int reclaim_wake_efd;

  // The one thread that waits on epfd, and the thread count the caller asked
  // for. With one thread the poller also runs every callback and the reclaim.
  ccol__thread_id poller_thread;
  size_t num_reactor_threads;

  // NULL with one reactor thread, else num_reactor_threads - 1 workers that run
  // the callbacks. The poller only collects readiness and submits jobs; the
  // pool's queue is unbounded, so a submit never blocks it.
  cthrpool dispatch_pool;

  // Guards only the one-shot shutdown coordination: shutdown_started, joined
  // and joined_cv. destroy_claimed, under the write side of the loop table
  // lock, says a destroy owns the teardown while the handle still resolves.
  ccol__mutex shutdown_lock;
  ccol__cond_var joined_cv;
  bool shutdown_started;
  bool joined;
  bool destroy_claimed;
  _Atomic bool shutting_down;

  // Lock-free stacks of retired entries and registrations waiting for the
  // reclaim. Freeing at once would leave a stale batch, a running job or a
  // caller with a freed pointer.
  _Atomic(event_entry *) pending_entry_frees;

  _Atomic(ceventloop_reg_s *) pending_reg_frees;

  // The loop's own generation-tagged table for the public ceventloop_reg, the
  // same design as the loop table. It's what lets a public call detect a
  // removed or stale registration without touching memory that may be freed:
  // remove frees the slot and bumps its generation at once, before the reg
  // itself is reclaimed. Read-write, since every pause, resume and modify
  // resolves through it. reg_lost_indices counts released slots the free list
  // couldn't take, as in the loop table.
  ccol__rw_lock reg_slot_rwlock;
  cvec reg_slots;
  cvec reg_free_indices;
  size_t reg_lost_indices;

  // The reclaim epoch. The poller bumps it once between batches; a deferred
  // item records it in defer_gen and is free of any batch once defer_gen <
  // poller_batch_gen. 64 bits, so it never wraps.
  _Atomic uint64_t poller_batch_gen;

  // Makes the generation of each new entry, starting at 1 (0 means none).
  _Atomic uint64_t fd_generation_counter;

  size_t max_events_per_wait;

  // The poller's event buffer of max_events_per_wait entries, allocated before
  // the thread starts so the thread has no step that can fail.
  ccol_poll_event *poller_events;

  // The lock stripes of the registry.
  ceventloop_stripe *stripes;
  size_t num_stripes;

  // Round-robin cursor that gives each queue registration a stripe. A queue's
  // entry is never looked up by another call, so any deterministic choice
  // works.
  _Atomic size_t next_queue_stripe;

  // Live registrations, for ceventloop_reg_count.
  _Atomic size_t reg_count;

  ccol_memmgmt_procs *m_procs;

  // Pins of resolved loop pointers. Pin and unpin are bare atomics, so the hot
  // calls take no global lock; the destroy polls until it reaches 0.
  _Atomic size_t pending_resolve_count;

  // The loop's public handle, written once before the create returns. Callbacks
  // get it as their loop argument, so a call back into the loop resolves like
  // any other.
  ceventloop self_handle;

#if CCOL_FORK_SAFETY_REQUIRED
  // Set by the child fork handler on every inherited loop. Then the shutdown
  // neither joins the poller (this process never created it) nor writes
  // shutdown_efd (shared with the parent's poller).
  _Atomic bool foreign_since_fork;
#endif

#ifdef RUNNING_UNIT_TESTS
  // Test-only counts of finished poller waits (to spot a busy spin) and of
  // entries the reclaim freed.
  _Atomic uint64_t poller_iterations_for_tests;
  _Atomic uint64_t entries_reclaimed_for_tests;
#endif
};

#if CCOL_FORK_SAFETY_REQUIRED
// The prepare handler of fork(). fork() copies only the calling thread, so a
// lock that another thread holds would stay locked for ever in the child; this
// takes every lock of the module first, so fork() happens when nobody holds
// one. The order matches every nesting in this file: The queue registry mutex
// and the loop table lock (write side), then for each live loop its
// shutdown_lock, reg_slot_rwlock (write side) and stripe locks, then every
// queue mutex once, then every queue registration's wait_mtx. Entry dispatch
// locks are left out, since taking one under a stripe lock reverses the order
// the dispatch uses; a child never dispatches an inherited loop anyway. Only
// slots with in_use are walked, which covers a loop whose destroy is still
// draining.
static void cthreadcomm_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  ccol__atfork_order_record(ccol__atfork_module_cthreadcomm);
#endif
  ccol__mutex_lock(queue_mutex_registry.mutex);
  ccol__rw_lock_wrlock(ceventloop_slot_table.rwlock);

  size_t n_loops = event_loop_slot_count_locked();

  for (size_t i = 0; i < n_loops; i++) {
    ceventloop_slot *slot =
        (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct ceventloop__s *loop = slot->ptr;
    ccol__mutex_lock(loop->shutdown_lock);
    ccol__rw_lock_wrlock(loop->reg_slot_rwlock);
    for (size_t s = 0; s < loop->num_stripes; s++) {
      ccol__mutex_lock(loop->stripes[s].lock);
    }
  }

  size_t n_addrs = queue_mutex_registry.addrs
                       ? cvector_elem_count(queue_mutex_registry.addrs)
                       : 0;
  for (size_t k = 0; k < n_addrs; k++) {
    ccol__mutex *m = ((queue_mutex_registry_entry *)cvector_at(
                          queue_mutex_registry.addrs, k))
                         ->mutex;
    ccol__mutex_lock(*m);
  }

  for (size_t i = 0; i < n_loops; i++) {
    ceventloop_slot *slot =
        (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct ceventloop__s *loop = slot->ptr;
    for (size_t s = 0; s < loop->num_stripes; s++) {
      for (ceventloop_reg_s *reg = loop->stripes[s].queue_regs_head; reg;
           reg = reg->loop_list_next) {
        ccol__mutex_lock(reg->wait_mtx);
      }
    }
  }
}

// Releases what the prepare handler took, in reverse phases, in the parent
// (is_child false) or the child. Nothing could change the walked structures in
// between, since every lock a change needs is held. The two rwlocks are
// re-initialised in the child instead of unlocked, because a glibc rwlock
// unlocks a writer only for the thread ID that locked it and the child's
// thread has a new ID. In the child every live loop also becomes
// foreign_since_fork, its pending_resolve_count goes back to 0 (a vanished
// thread may have left a pin), and its epfd is replaced by a new empty set, so
// the child's epoll_ctl calls don't edit the parent's live set. A failed
// replacement keeps the shared set.
static void cthreadcomm_atfork_release_impl(bool is_child) {
  size_t n_loops = event_loop_slot_count_locked();

  for (size_t i = 0; i < n_loops; i++) {
    ceventloop_slot *slot =
        (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct ceventloop__s *loop = slot->ptr;

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
      for (ceventloop_reg_s *reg = loop->stripes[s].queue_regs_head; reg;
           reg = reg->loop_list_next) {
        ccol__mutex_unlock(reg->wait_mtx);
      }
    }
  }

  size_t n_addrs = queue_mutex_registry.addrs
                       ? cvector_elem_count(queue_mutex_registry.addrs)
                       : 0;
  for (size_t k = 0; k < n_addrs; k++) {
    ccol__mutex *m = ((queue_mutex_registry_entry *)cvector_at(
                          queue_mutex_registry.addrs, k))
                         ->mutex;
    ccol__mutex_unlock(*m);
  }

  for (size_t i = 0; i < n_loops; i++) {
    ceventloop_slot *slot =
        (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct ceventloop__s *loop = slot->ptr;
    for (size_t s = 0; s < loop->num_stripes; s++) {
      ccol__mutex_unlock(loop->stripes[s].lock);
    }
    if (is_child) {
      if (ccol__rw_lock_reinit_in_child(loop->reg_slot_rwlock) != 0)
        ccol__fatal_err(
            "ceventloop atfork release: failed to reinit reg_slot_rwlock");
    } else {
      ccol__rw_lock_unlock(loop->reg_slot_rwlock);
    }
    ccol__mutex_unlock(loop->shutdown_lock);
  }

  if (is_child) {
    if (ccol__rw_lock_reinit_in_child(ceventloop_slot_table.rwlock) != 0)
      ccol__fatal_err(
          "ceventloop atfork release: failed to reinit "
          "ceventloop_slot_table "
          "rwlock");
  } else {
    ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
  }
  ccol__mutex_unlock(queue_mutex_registry.mutex);
}

// The parent fork handler: Releases every lock the prepare handler took.
static void cthreadcomm_atfork_release(void) {
  cthreadcomm_atfork_release_impl(false);
}

// The child fork handler: Releases the locks and detaches every inherited loop
// from the parent (see cthreadcomm_atfork_release_impl). It runs before fork()
// returns, so before any code of the child can reach these loops.
static void cthreadcomm_atfork_child_release(void) {
  cthreadcomm_atfork_release_impl(true);
}
#endif

// Resolves a loop handle and pins the loop against a concurrent destroy. Every
// public call that takes a ceventloop starts here, so a stale handle is
// detected before anything is touched. Gives NULL for 0, garbage, a free slot
// or a wrong generation. On success the caller calls ceventloop_resolve_unpin()
// once when it's done. Takes the read side of the loop table lock; the pin is a
// bare atomic increment, safe because the destroy releases the slot only under
// the write side.
static struct ceventloop__s *ceventloop_resolve(ceventloop h) {
  ccol__call_once(ceventloop_slot_table.once,
                  ceventloop_slot_table_init_globals);
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol__rw_lock_rdlock(ceventloop_slot_table.rwlock);
  struct ceventloop__s *raw = NULL;
  if (idx < event_loop_slot_count_locked()) {
    ceventloop_slot *slot =
        (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
  return raw;
}

// Drops the pin of ceventloop_resolve(). A bare atomic decrement: The destroy
// polls the count, so there's no waiter to signal.
static void ceventloop_resolve_unpin(struct ceventloop__s *raw) {
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: Makes the next push onto the loop free list fail as without
// memory, to reach the lost-index recovery.
static _Atomic bool g_fail_next_loop_free_index_push = false;

// Test-only: Arms the failing free list push.
void ceventloop__force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_fail_next_loop_free_index_push, true);
}
#endif

// Puts idx back on the loop free list for reuse by a later create. A push that
// can't allocate counts the index in lost_indices instead of stranding it. The
// caller holds the write lock.
static void ceventloop_free_index_release_locked(uint32_t idx) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_fail_next_loop_free_index_push, false)) {
    ceventloop_slot_table.lost_indices++;
    return;
  }
#endif
  if (cvector_push_back(ceventloop_slot_table.free_indices, &idx) !=
      ccol_success)
    ceventloop_slot_table.lost_indices++;
}

// Finds a slot that a failed free list push stranded, so an acquire reuses it
// instead of growing the table. With the free list empty, a slot with no ptr
// and not in use can only be such a slot (a destroy keeps ptr until its last
// step). It zeroes the count when it finds none, so the scan doesn't repeat.
// Gives the index in *out_idx. Out of line, since only a non-zero count reaches
// it. The caller holds the write lock.
static __attribute__((noinline)) bool ceventloop_reclaim_lost_index_locked(
    uint32_t *out_idx) {
  size_t slot_count = cvector_elem_count(ceventloop_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    ceventloop_slot *slot =
        (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      ceventloop_slot_table.lost_indices--;
      return true;
    }
  }
  ceventloop_slot_table.lost_indices = 0;
  return false;
}

// Gives loop a slot of the process-wide table and gives its handle, or 0 when
// no slot can be allocated or the exit destructor released the table.
// ceventloop_create_mp calls it as its last step. It reuses a free index, then
// a lost one, and grows the table only after that; the generation rises on
// every acquire. Takes the write lock.
static ceventloop ceventloop_handle_slot_acquire(struct ceventloop__s *loop) {
  ccol__call_once(ceventloop_slot_table.once,
                  ceventloop_slot_table_init_globals);
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
  ccol__rw_lock_wrlock(ceventloop_slot_table.rwlock);
  if (!ceventloop_slot_table.slots) {
    ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
    return 0;
  }
  uint32_t idx;
  ceventloop_slot *slot;
  if (cvector_elem_count(ceventloop_slot_table.free_indices) > 0) {
    cvector_pop_back(ceventloop_slot_table.free_indices, &idx);
    slot = (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, idx);
  } else if (ceventloop_slot_table.lost_indices > 0 &&
             ceventloop_reclaim_lost_index_locked(&idx)) {
    slot = (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, idx);
  } else {
    ceventloop_slot fresh = {0};
    if (cvector_push_back(ceventloop_slot_table.slots, &fresh) !=
        ccol_success) {
      ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
      return 0;
    }
    idx = (uint32_t)cvector_elem_count(ceventloop_slot_table.slots) - 1;
    slot = (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0) slot->generation++;
  slot->ptr = loop;
  slot->in_use = true;
  ceventloop h = ((ceventloop)idx << 32) | (ceventloop)slot->generation;
  ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
  return h;
}

// Maps an fd to its lock stripe, so every call for one fd takes the same
// stripe lock. Knuth's multiplicative hash reduced from its high bits, which
// depend on every bit of the fd (the low bits would cluster like
// fd % num_stripes). Only fds use it; queue registrations go round-robin.
static size_t stripe_index(size_t num_stripes, int fd) {
  uint32_t h = (uint32_t)fd * 2654435761u;
#if SIZE_MAX > UINT32_MAX
  if (num_stripes > UINT32_MAX) return (size_t)h;
#endif
  return (size_t)(((uint64_t)h * (uint64_t)num_stripes) >> 32);
}

// stripe_index() for the stripe count of loop.
static size_t stripe_index_for_fd(struct ceventloop__s *loop, int fd) {
  return stripe_index(loop->num_stripes, fd);
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: Exposes stripe_index().
size_t ceventloop__test_stripe_index(size_t num_stripes, int fd) {
  return stripe_index(num_stripes, fd);
}
#endif

// Finds the entry of fd in one stripe's fd index, or NULL, for the add,
// modify and remove paths. The pointer is read with memcpy as a second line of
// defence, like the other chmap users. The caller holds the stripe lock.
static event_entry *fd_registry_find(ceventloop_stripe *stripe, int fd) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  cmap_pair val_pair;
  if (chashmap_get_elem_ref(stripe->fd_index, &key_pair, &val_pair) !=
      ccol_success) {
    return NULL;
  }
  event_entry *entry;
  memcpy(&entry, val_pair.ptr, sizeof(entry));
  return entry;
}

// Inserts fd -> entry into the stripe's index when ceventloop_add_fd() makes a
// new entry, after a find said fd is absent. Gives false when the map can't
// grow. The caller holds the stripe lock.
static bool fd_registry_insert(ceventloop_stripe *stripe, int fd,
                               event_entry *entry) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  cmap_pair val_pair = {.ptr = &entry, .size = sizeof(entry)};
  return chashmap_insert_elem(stripe->fd_index, &key_pair, &val_pair) ==
         ccol_success;
}

// Removes fd from the stripe's index when its last registration goes (or a new
// entry's publish fails). The caller holds the stripe lock.
static void fd_registry_remove(ceventloop_stripe *stripe, int fd) {
  cmap_pair key_pair = {.ptr = &fd, .size = sizeof(fd)};
  chashmap_delete_elem(stripe->fd_index, &key_pair);
}

// Links and unlinks a queue registration in its stripe's list, which the
// destroy and the fork handlers walk. The caller holds the stripe lock.
static void loop_queue_list_add(ceventloop_stripe *stripe,
                                ceventloop_reg_s *reg) {
  reg->loop_list_prev = NULL;
  reg->loop_list_next = stripe->queue_regs_head;
  if (stripe->queue_regs_head) stripe->queue_regs_head->loop_list_prev = reg;
  stripe->queue_regs_head = reg;
}

static void loop_queue_list_remove(ceventloop_stripe *stripe,
                                   ceventloop_reg_s *reg) {
  if (reg->loop_list_prev)
    reg->loop_list_prev->loop_list_next = reg->loop_list_next;
  else
    stripe->queue_regs_head = reg->loop_list_next;
  if (reg->loop_list_next)
    reg->loop_list_next->loop_list_prev = reg->loop_list_prev;
}

// Gives the mutex of the queue sel names and the head and rotor of the waiter
// list for its direction. The registration, deregistration and dispatch paths
// use it so they share one idea of which list a selectable lives on. sel is a
// ccircq or cdynmq; a channel selectable is already a ccircq.
static void queue_sel_locate(cselectable *sel, ccol__mutex **out_mtx,
                             ccol_sel_waiter ***out_head,
                             ccol_sel_waiter ***out_rotor) {
  if (sel->type == cselectable_type_ccircq) {
    ccircq *cq = sel->cq;
    *out_mtx = &cq->mutex;
    *out_head = (sel->dir == cselect_read) ? &cq->sel_read_waiters_head
                                           : &cq->sel_write_waiters_head;
    *out_rotor =
        (sel->dir == cselect_read) ? &cq->sel_read_rotor : &cq->sel_write_rotor;
  } else {
    cdynmq *dq = sel->dq;
    *out_mtx = &dq->mutex;
    *out_head = (sel->dir == cselect_read) ? &dq->sel_read_waiters_head
                                           : &dq->sel_write_waiters_head;
    *out_rotor =
        (sel->dir == cselect_read) ? &dq->sel_read_rotor : &dq->sel_write_rotor;
  }
}

// Give the queue a queue selectable names and that queue's dispatch reference
// counter, for the dispatch mark and the destroy wait.
static void *queue_sel_queue(cselectable *sel) {
  return (sel->type == cselectable_type_ccircq) ? (void *)sel->cq
                                                : (void *)sel->dq;
}

static _Atomic size_t *queue_sel_dispatch_refs(cselectable *sel) {
  return (sel->type == cselectable_type_ccircq) ? &sel->cq->dispatch_refs
                                                : &sel->dq->dispatch_refs;
}

// Gives the detached registration count of the queue sel names, which the
// loop shutdown raises and the remove lowers. The caller holds the queue's
// mutex.
static size_t *queue_sel_detached_count(cselectable *sel) {
  return (sel->type == cselectable_type_ccircq) ? &sel->cq->detached_loop_regs
                                                : &sel->dq->detached_loop_regs;
}

// Claims a dispatch reference on the queue sel names, so its destroy waits for
// this callback. The caller holds the stripe lock under which it just found the
// registration not removed, and the remove sets that flag under the same lock,
// so once the remove returns no reference can be claimed. Only a dispatch about
// to run its callback claims one (on a pool worker, when the job starts): A
// queued job holding one could make a destroy on a worker wait for a job stuck
// behind that same worker.
static void queue_dispatch_ref_acquire(cselectable *sel) {
  atomic_fetch_add(queue_sel_dispatch_refs(sel), (size_t)1);
}

// Gives the message count of the queue sel names, for the progress snapshot of
// a dispatch. The caller holds the queue's mutex.
static size_t queue_sel_msg_count(cselectable *sel) {
  return (sel->type == cselectable_type_ccircq) ? sel->cq->msg_count
                                                : sel->dq->msg_count;
}

// Consumes a queue registration's outstanding wake right before its dispatch
// runs: It clears wait_ready, takes the hop count into *hops and drains the
// bridge eventfd in one critical section under wait_mtx. notify_waiter() works
// under the same mutex, so a notify lands either before (its message is there
// for the callback) or after (it writes again). When queue_count isn't NULL it
// also snapshots the message count under the queue's mutex, taking wait_mtx
// before it lets the queue go, so the cascade step can tell whether the
// callback moved a message. The caller holds the stripe lock.
static void queue_reg_consume_wake(ceventloop_reg_s *reg, size_t *queue_count,
                                   uint32_t *hops) {
  if (queue_count) {
    ccol__mutex *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
    ccol__mutex_lock(*q_mtx);
    *queue_count = queue_sel_msg_count(&reg->sel);
    ccol__mutex_lock(reg->wait_mtx);
    ccol__mutex_unlock(*q_mtx);
  } else {
    ccol__mutex_lock(reg->wait_mtx);
  }
  reg->wait_ready = false;
  if (hops) *hops = reg->waiter_node.fwd_hops;
  reg->waiter_node.fwd_hops = 0;
  eventfd_drain(reg->bridge_efd);
  ccol__mutex_unlock(reg->wait_mtx);
}

// Drops a dispatch reference once a queue callback and its cascade step are
// done. Nothing may touch the queue after it, since a waiting destroy can go on
// at once.
static void queue_dispatch_ref_release(cselectable *sel) {
  atomic_fetch_sub(queue_sel_dispatch_refs(sel), (size_t)1);
}

// One slot of a loop's registration table, behind a public ceventloop_reg. The
// generation rises on every acquire and skips 0.
typedef struct {
  ceventloop_reg_s *ptr;
  uint32_t generation;
  bool in_use;
} ceventloop_reg_slot;

#ifdef RUNNING_UNIT_TESTS
// Test-only: Makes the next registration slot acquire fail, which the shared
// allocator can't do in isolation.
static _Atomic bool g_force_next_reg_slot_acquire_failure = false;

// Test-only: The loop's registration count when the forced failure fired. A 0
// proves the slot is taken before the registration is wired.
static _Atomic size_t g_forced_slot_acquire_failure_reg_count_snapshot = 0;

// Test-only: Arms the forced slot failure.
void ceventloop__test_force_next_reg_slot_acquire_failure(void) {
  atomic_store(&g_force_next_reg_slot_acquire_failure, true);
}

// Test-only: Reads the snapshot.
size_t ceventloop__test_last_forced_slot_acquire_failure_reg_count(void) {
  return atomic_load(&g_forced_slot_acquire_failure_reg_count_snapshot);
}

// Test-only hook that ceventloop_add calls once the registration is live. No
// initializer, since Clang 11 refuses one on an atomic function pointer.
static void (*_Atomic g_evl_add_before_return_hook)(void);

// Test-only: Installs the hook, NULL to remove it.
void ceventloop__test_set_add_before_return_hook(void (*hook)(void)) {
  atomic_store(&g_evl_add_before_return_hook, hook);
}

// Test-only: Resolves the loop once, write-locks its reg_slot_rwlock and gives
// the pinned loop, so a test can hold the lock across a fork(). The unlock
// takes that pointer, since resolving again could deadlock against a fork that
// holds the table lock while it waits for this one.
void *ceventloop__test_wrlock_reg_slot_for_tests(ceventloop loop) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return NULL;
  ccol__rw_lock_wrlock(raw->reg_slot_rwlock);
  return raw;
}

// Test-only: Unlocks what the call above locked and drops its pin.
void ceventloop__test_wrunlock_reg_slot_for_tests(void *resolved_loop) {
  struct ceventloop__s *raw = (struct ceventloop__s *)resolved_loop;
  if (!raw) return;
  ccol__rw_lock_unlock(raw->reg_slot_rwlock);
  ceventloop_resolve_unpin(raw);
}
#endif

#ifdef RUNNING_UNIT_TESTS
// Test-only: Makes the next registration free list push fail.
static _Atomic bool g_fail_next_reg_free_index_push = false;

// Test-only: Arms the failing push.
void ceventloop__force_next_reg_free_index_push_failure_for_tests(void) {
  atomic_store(&g_fail_next_reg_free_index_push, true);
}

// Test-only: How many registration slots the loop has ever allocated.
size_t ceventloop__reg_slot_count_for_tests(ceventloop loop) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return 0;
  ccol__rw_lock_rdlock(raw->reg_slot_rwlock);
  size_t n = cvector_elem_count(raw->reg_slots);
  ccol__rw_lock_unlock(raw->reg_slot_rwlock);
  ceventloop_resolve_unpin(raw);
  return n;
}
#endif

// Finds a registration slot that a failed free list push stranded, so the
// acquire reuses it instead of growing the table; it works like the loop table
// version. Gives the index in *out_idx. The caller holds the write side of
// reg_slot_rwlock.
static __attribute__((noinline)) bool ceventloop_reg_reclaim_lost_index_locked(
    struct ceventloop__s *loop, uint32_t *out_idx) {
  size_t slot_count = cvector_elem_count(loop->reg_slots);
  for (size_t i = 0; i < slot_count; i++) {
    ceventloop_reg_slot *slot =
        (ceventloop_reg_slot *)cvector_at(loop->reg_slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      loop->reg_lost_indices--;
      return true;
    }
  }
  loop->reg_lost_indices = 0;
  return false;
}

// Gives reg a slot of the loop's registration table and gives the public
// handle, or 0 when no slot can be allocated. ceventloop_add calls it before it
// wires reg into any registry, so a failure has nothing to unwind. It reuses a
// free index, then a lost one, then grows the table, and sets
// reg->self_slot_idx and reg->self_handle. Takes the write side of
// reg_slot_rwlock, since a growth can move the table under a reader.
static ceventloop_reg ceventloop_reg_slot_acquire(struct ceventloop__s *loop,
                                                  ceventloop_reg_s *reg) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_force_next_reg_slot_acquire_failure)) {
    atomic_store(&g_force_next_reg_slot_acquire_failure, false);
    atomic_store(&g_forced_slot_acquire_failure_reg_count_snapshot,
                 atomic_load(&loop->reg_count));
    return 0;
  }
#endif
  ccol__rw_lock_wrlock(loop->reg_slot_rwlock);
  uint32_t idx;
  ceventloop_reg_slot *slot;
  if (cvector_elem_count(loop->reg_free_indices) > 0) {
    cvector_pop_back(loop->reg_free_indices, &idx);
    slot = (ceventloop_reg_slot *)cvector_at(loop->reg_slots, idx);
  } else if (loop->reg_lost_indices > 0 &&
             ceventloop_reg_reclaim_lost_index_locked(loop, &idx)) {
    slot = (ceventloop_reg_slot *)cvector_at(loop->reg_slots, idx);
  } else {
    ceventloop_reg_slot fresh = {0};
    if (cvector_push_back(loop->reg_slots, &fresh) != ccol_success) {
      ccol__rw_lock_unlock(loop->reg_slot_rwlock);
      return 0;
    }
    idx = (uint32_t)cvector_elem_count(loop->reg_slots) - 1;
    slot = (ceventloop_reg_slot *)cvector_at(loop->reg_slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0) slot->generation++;
  slot->ptr = reg;
  slot->in_use = true;
  reg->self_slot_idx = idx;
  ceventloop_reg h =
      ((ceventloop_reg)idx << 32) | (ceventloop_reg)slot->generation;
  reg->self_handle = h;
  ccol__rw_lock_unlock(loop->reg_slot_rwlock);
  return h;
}

// Resolves a registration handle of loop and pins the registration against a
// concurrent free, the per-loop twin of ceventloop_resolve(). Every public call
// that takes a ceventloop_reg uses it, so a removed or stale handle gives NULL
// without a dereference. On success the caller calls
// ceventloop_reg_resolve_unpin() once. Takes the read side of reg_slot_rwlock,
// so resolves run in parallel; the pin happens under it, and the remove frees
// the slot only under the write side.
static ceventloop_reg_s *ceventloop_reg_resolve(struct ceventloop__s *loop,
                                                ceventloop_reg h) {
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol__rw_lock_rdlock(loop->reg_slot_rwlock);
  ceventloop_reg_s *raw = NULL;
  if (idx < cvector_elem_count(loop->reg_slots)) {
    ceventloop_reg_slot *slot =
        (ceventloop_reg_slot *)cvector_at(loop->reg_slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  ccol__rw_lock_unlock(loop->reg_slot_rwlock);
  return raw;
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: The armed sleep of the next unpin, between its read of removed
// and its decrement, which lets a test land a remove inside that window.
static _Atomic uint32_t g_delay_next_reg_resolve_unpin_ms = 0;

// Test-only: Arms the unpin delay.
void ceventloop__test_delay_next_reg_resolve_unpin_ms(uint32_t ms) {
  atomic_store(&g_delay_next_reg_resolve_unpin_ms, ms);
}

// Test-only: true once an unpin has taken the armed delay (it's taken after the
// read of removed, with the pin still held).
bool ceventloop__test_reg_resolve_unpin_delay_taken_for_tests(void) {
  return atomic_load(&g_delay_next_reg_resolve_unpin_ms) == 0u;
}
#endif

// Drops the pin of ceventloop_reg_resolve(). When the registration is removed,
// has an on_removed and this was the last pin, it pings the poller so the
// notice comes quickly. removed is read before the decrement, while the pin
// still makes raw safe; a remove that lands in between makes that read stale,
// and the bounded reclaim retry then covers the missed ping. Reading after the
// decrement would be a use-after-free.
static void ceventloop_reg_resolve_unpin(struct ceventloop__s *loop,
                                         ceventloop_reg_s *raw) {
  bool maybe_needs_wake =
      atomic_load(&raw->removed) && raw->handlers.on_removed;

#ifdef RUNNING_UNIT_TESTS
  uint32_t delay_ms = atomic_exchange(&g_delay_next_reg_resolve_unpin_ms, 0u);
  if (delay_ms > 0) {
    struct timespec ts = {.tv_sec = delay_ms / 1000,
                          .tv_nsec = (long)(delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
  }
#endif

  int prev = atomic_fetch_sub(&raw->pending_resolve_count, 1);

  if (prev == 1 && maybe_needs_wake) eventfd_notify(loop->reclaim_wake_efd);
}

// Frees reg's slot (not in use, ptr NULL) so every later resolve of its handle
// fails at once, and puts the index on the free list; a push that can't
// allocate counts it in reg_lost_indices. ceventloop_remove calls it the moment
// it removes reg, well before the struct itself is reclaimed; the two have
// separate lifetimes, so the index can be reused at once. Takes the write side
// of reg_slot_rwlock.
static void ceventloop_reg_slot_release(struct ceventloop__s *loop,
                                        ceventloop_reg_s *reg) {
  ccol__rw_lock_wrlock(loop->reg_slot_rwlock);
  ceventloop_reg_slot *slot =
      (ceventloop_reg_slot *)cvector_at(loop->reg_slots, reg->self_slot_idx);
  slot->in_use = false;
  slot->ptr = NULL;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_fail_next_reg_free_index_push, false)) {
    loop->reg_lost_indices++;
    ccol__rw_lock_unlock(loop->reg_slot_rwlock);
    return;
  }
#endif
  if (cvector_push_back(loop->reg_free_indices, &reg->self_slot_idx) !=
      ccol_success) {
    loop->reg_lost_indices++;
  }
  ccol__rw_lock_unlock(loop->reg_slot_rwlock);
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: Makes the next wait mutex init of a queue registration fail.
static _Atomic bool g_fail_next_reg_sync_init = false;

// Test-only: Arms the failing init.
void ceventloop__test_fail_next_reg_sync_init(void) {
  atomic_store(&g_fail_next_reg_sync_init, true);
}
#endif

// Allocates and fills a registration for ceventloop_add. A queue registration
// also gets its wait_mtx and wait_cond here, before anything else can fail, so
// ceventloop_reg_free can always destroy them. Gives NULL with *rv_out set to
// ccol_not_enough_memory or, when an init fails (reg is then freed directly),
// ccol_unexpected_failure.
static ceventloop_reg_s *ceventloop_reg_create(struct ceventloop__s *loop,
                                               cselectable sel,
                                               ceventloop_handlers handlers,
                                               void *arg, ccol_retval *rv_out) {
  ceventloop_reg_s *reg =
      ccol__mem_calloc(loop->m_procs, 1, sizeof(ceventloop_reg_s));
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
  if (sel.type != cselectable_type_fd) {
    bool mutex_failed;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_exchange(&g_fail_next_reg_sync_init, false))
      mutex_failed = true;
    else
#endif
      mutex_failed = ccol__mutex_init(reg->wait_mtx) != 0;
    if (mutex_failed) {
      ccol__mem_free(loop->m_procs, reg);
      *rv_out = ccol_unexpected_failure;
      return NULL;
    }
    if (ccol__cond_var_init(reg->wait_cond) != 0) {
      ccol__mutex_destroy(reg->wait_mtx);
      ccol__mem_free(loop->m_procs, reg);
      *rv_out = ccol_unexpected_failure;
      return NULL;
    }
  }
  return reg;
}

// Frees a registration: It calls on_removed when the registration was ever
// wired (owning_entry set), so the notice comes exactly once for every real
// registration, then destroys wait_mtx and wait_cond and closes the bridge
// eventfd of a queue registration. It's the only place that tears those down,
// for the reclaim, the destroy and a failed add, so nothing is destroyed twice.
// No lock of this module is held, so on_removed may take any lock of yours.
static void ceventloop_reg_free(struct ceventloop__s *loop,
                                ceventloop_reg_s *reg) {
  if (reg->owning_entry && reg->handlers.on_removed)
    reg->handlers.on_removed(reg->arg);

  if (reg->sel.type != cselectable_type_fd) {
    ccol__mutex_destroy(reg->wait_mtx);
    ccol__cond_var_destroy(reg->wait_cond);
    if (reg->bridge_efd >= 0) close(reg->bridge_efd);
  }
  ccol__mem_free(loop->m_procs, reg);
}

// Gives the interest bits one direction of an fd entry adds to the combined
// mask, 0 for no registration or a paused or muted one. IN and OUT come only
// from a real on_readable or on_writable, because readiness that nothing
// consumes would make level-triggered epoll spin (an error-only registration on
// a healthy socket would). ERR and HUP always go in, and a read direction adds
// RDHUP, the only bit that reports a plain TCP close to a registration with no
// reader.
static uint32_t fd_reg_interest_bits(ceventloop_reg_s *reg, cselect_dir dir) {
  if (!reg || atomic_load(&reg->paused) || reg->muted) return 0;
  uint32_t bits = (uint32_t)(CCOL_POLL_ERR | CCOL_POLL_HUP);
  if (dir == cselect_read) {
    bits |= (uint32_t)CCOL_POLL_RDHUP;
    if (reg->handlers.on_readable) bits |= (uint32_t)CCOL_POLL_IN;
  } else if (reg->handlers.on_writable) {
    bits |= (uint32_t)CCOL_POLL_OUT;
  }
  return bits;
}

static size_t ceventloop_entry_alloc_size(const struct ceventloop__s *loop);

// Registers reg's direction of an fd in stripe idx, for ceventloop_add. A new
// fd gets a new entry (with a new generation) and an ADD; a second direction
// joins the existing entry and the combined mask is applied with MOD, or ADD
// when the entry was taken out of the set (epoll_added false). The mask is
// one-shot only with a dispatch pool, where the pool re-arms after each job;
// with one thread nothing re-arms, so one-shot would lose every later event. A
// new entry goes into the fd index before epoll_ctl publishes it, because once
// published only the deferred reclaim may free it. Gives ccol_not_permitted
// when the direction is taken, ccol_not_enough_memory, or
// ccol_unexpected_failure. On success it sets owning_entry, stripe_idx and
// generation of reg. The caller holds the stripe lock.
static ccol_retval ceventloop_add_fd(struct ceventloop__s *loop, size_t idx,
                                     ceventloop_reg_s *reg) {
  ceventloop_stripe *stripe = &loop->stripes[idx];
  int fd = reg->sel.fd;
  event_entry *entry = fd_registry_find(stripe, fd);
  bool new_entry = (entry == NULL);

  if (new_entry) {
    entry =
        ccol__mem_calloc(loop->m_procs, 1, ceventloop_entry_alloc_size(loop));
    if (!entry) return ccol_not_enough_memory;
    entry->is_fd = true;
    entry->fd = fd;
    entry->stripe_idx = idx;
    entry->as.fd.read_reg = NULL;
    entry->as.fd.write_reg = NULL;
    if (ccol__mutex_init(entry->dispatch_lock) != 0) {
      ccol__mem_free(loop->m_procs, entry);
      return ccol_unexpected_failure;
    }
    atomic_init(&entry->removed, false);
    atomic_init(&entry->refcount, (size_t)0);
    entry->generation = atomic_fetch_add(&loop->fd_generation_counter, 1) + 1;
  }

  ceventloop_reg_s **slot = (reg->sel.dir == cselect_read)
                                ? &entry->as.fd.read_reg
                                : &entry->as.fd.write_reg;
  if (*slot != NULL) {
    if (new_entry) {
      ccol__mutex_destroy(entry->dispatch_lock);
      ccol__mem_free(loop->m_procs, entry);
    }
    return ccol_not_permitted;
  }
  *slot = reg;

  uint32_t mask = loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0;
  mask |= fd_reg_interest_bits(entry->as.fd.read_reg, cselect_read);
  mask |= fd_reg_interest_bits(entry->as.fd.write_reg, cselect_write);

  ccol_poll_event ev;
  ev.data.ptr = entry;
  ev.events = mask;
  if (new_entry && !fd_registry_insert(stripe, fd, entry)) {
    *slot = NULL;
    ccol__mutex_destroy(entry->dispatch_lock);
    ccol__mem_free(loop->m_procs, entry);
    return ccol_not_enough_memory;
  }

  int ctl_op = entry->epoll_added ? CCOL_POLL_CTL_MOD : CCOL_POLL_CTL_ADD;
  bool want_in = (mask & CCOL_POLL_IN) != 0;
  if (ctl_op == CCOL_POLL_CTL_MOD && want_in != entry->armed_in)
    ev.events |= CCOL_POLL_READ_MODE_CHANGE;
  if (ccol_poll_ctl(loop->epfd, ctl_op, fd, &ev, &entry->poll_info) < 0) {
    *slot = NULL;
    if (new_entry) {
      fd_registry_remove(stripe, fd);
      ccol__mutex_destroy(entry->dispatch_lock);
      ccol__mem_free(loop->m_procs, entry);
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

// Says whether the queue sel names is ready for sel's direction: A message
// for read, room or sends off for write. It's the one definition of ready for a
// queue, used by the add, the cascade and the deregister paths so they never
// disagree. The caller holds the queue's mutex.
static bool queue_sel_is_ready(cselectable *sel) {
  if (sel->type == cselectable_type_ccircq) {
    ccircq *cq = sel->cq;
    return (sel->dir == cselect_read)
               ? cq->msg_count > 0
               : (cq->msg_count < cq->max_size || cq->writing_disabled);
  }
  cdynmq *dq = sel->dq;
  return (sel->dir == cselect_read)
             ? dq->msg_count > 0
             : (dq->writing_disabled || dq->msg_count < ccol_max_elem_count);
}

// Wires a queue registration for ceventloop_add: It creates the bridge
// eventfd, adds it to the poll set and links the registration's waiter node
// into the queue's list for good, so every send, receive and state change of
// the queue wakes it. A queue that is already ready gets a wake through
// notify_waiter() at once, since the eventfd rings only on later notifies. On
// failure it closes only the eventfd; ceventloop_reg_free owns the rest. The
// caller holds the stripe lock, and this takes the queue's mutex.
static ccol_retval ceventloop_add_queue(struct ceventloop__s *loop,
                                        event_entry *entry,
                                        ceventloop_reg_s *reg) {
  reg->bridge_efd = ccol_wakefd_create();
  if (reg->bridge_efd < 0) {
    return ccol_unexpected_failure;
  }
  reg->waiter_node.efd = reg->bridge_efd;

  ccol_poll_event ev;
  ev.data.ptr = entry;
  ev.events = CCOL_POLL_IN | (loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0);
  if (ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_ADD, reg->bridge_efd, &ev, NULL) <
      0) {
    close(reg->bridge_efd);
    reg->bridge_efd = -1;
    reg->waiter_node.efd = -1;
    return ccol_unexpected_failure;
  }

  ccol__mutex *q_mtx;
  ccol_sel_waiter **q_head;
  ccol_sel_waiter **q_rotor;
  queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
  (void)q_rotor;

  ccol__mutex_lock(*q_mtx);
  sel_link_waiter(0, &reg->waiter_node, &reg->wait_mtx, &reg->wait_cond,
                  &reg->wait_ready, q_head);
  bool already_ready = queue_sel_is_ready(&reg->sel);
  if (already_ready) notify_waiter(&reg->waiter_node);
  ccol__mutex_unlock(*q_mtx);

  return ccol_success;
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: The armed delay of the next cascade step and the counts of steps
// begun and finished.
static _Atomic uint32_t g_queue_cascade_test_delay_us = 0;
static _Atomic uint64_t g_queue_cascade_begun_for_tests = 0;
static _Atomic uint64_t g_queue_cascade_finished_for_tests = 0;
#endif

// Decides who hears about the queue after reg's callback returned, since a
// registration stays linked and one eventfd write covers a whole burst. When
// the queue isn't ready, nobody is woken. When reg's dispatch moved a
// message (the count changed in its direction since queue_count_before), reg is
// woken again and its successor gets a forwarded wake, so a backlog drains and
// reaches the other listeners. When it moved nothing, the wake goes to the
// successor (or the head) with wake_hops + 1, and stops once the hops reach the
// list length, so listeners that all decline don't wake each other for ever. A
// modify that changed the direction counts as no progress. The stripe lock is
// held across the removed check and the queue's mutex: A removed registration
// may name a queue that's already destroyed, and the remove sets the flag under
// the same lock, while a live one keeps the queue destroy asserting. The caller
// holds the entry's dispatch lock and the collection's refcount.
static void ceventloop_queue_cascade_notify_next(struct ceventloop__s *loop,
                                                 ceventloop_reg_s *reg,
                                                 cselect_dir dispatched_dir,
                                                 size_t queue_count_before,
                                                 uint32_t wake_hops) {
  ceventloop_stripe *stripe = &loop->stripes[reg->stripe_idx];
  ccol__mutex_lock(stripe->lock);

#ifdef RUNNING_UNIT_TESTS
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

  if (!atomic_load(&reg->removed)) {
    ccol__mutex *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
    (void)q_rotor;

    ccol__mutex_lock(*q_mtx);
    if (queue_sel_is_ready(&reg->sel)) {
      ccol_sel_waiter *self = &reg->waiter_node;
      bool moved = false;
      if (reg->sel.dir == dispatched_dir) {
        size_t count_now = queue_sel_msg_count(&reg->sel);
        moved = (dispatched_dir == cselect_read)
                    ? count_now < queue_count_before
                    : count_now > queue_count_before;
      }
      if (moved) {
        notify_waiter(self);
        if (self->next) notify_waiter_forwarded(self->next, 1u);
      } else if (wake_hops < UINT32_MAX) {
        uint32_t next_hops = wake_hops + 1u;
        ccol_sel_waiter *target = self->next ? self->next : *q_head;
        if (target != self && sel_list_longer_than(*q_head, next_hops))
          notify_waiter_forwarded(target, next_hops);
      }
    }
    ccol__mutex_unlock(*q_mtx);
  }

#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_queue_cascade_finished_for_tests, (uint64_t)1);
#endif
  ccol__mutex_unlock(stripe->lock);
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: Arms the cascade delay.
void ceventloop__test_delay_next_queue_cascade_us(uint32_t us) {
  atomic_store(&g_queue_cascade_test_delay_us, us);
}

// Test-only: Reads the counts of cascade steps begun and finished.
void ceventloop__test_queue_cascade_counts(uint64_t *begun,
                                           uint64_t *finished) {
  if (begun) *begun = atomic_load(&g_queue_cascade_begun_for_tests);
  if (finished) *finished = atomic_load(&g_queue_cascade_finished_for_tests);
}
#endif

// Checks the selectable that ceventloop_add got: A known type and direction,
// an fd of at least 0 or a non-NULL queue. Gives ccol_success or
// ccol_invalid_args.
static ccol_retval ceventloop_validate_add_args(struct ceventloop__s *loop,
                                                cselectable *sel) {
  if (!loop) return ccol_invalid_args;
  if (sel->dir != cselect_read && sel->dir != cselect_write)
    return ccol_invalid_args;

  if (sel->type == cselectable_type_fd) {
    if (sel->fd < 0) return ccol_invalid_args;
  } else if (sel->type == cselectable_type_ccircq) {
    if (!sel->cq) return ccol_invalid_args;
  } else if (sel->type == cselectable_type_cdynmq) {
    if (!sel->dq) return ccol_invalid_args;
  } else {
    return ccol_invalid_args;
  }
  return ccol_success;
}

// Registers sel with the loop and gives the new handle, or CEVENTLOOP_REG_NULL
// with *err_str naming the failure class. It resolves the loop, builds the
// registration, takes its handle slot first and only then wires it into the
// fd or queue registry under the stripe lock: Once wired it can be dispatched,
// and a failure reported after that point could let the caller free an arg that
// a callback is about to use. That order also matches the fork prepare handler
// (reg_slot_rwlock before stripe locks). An fd's stripe comes from the fd, a
// queue's from the round-robin cursor. A loop that has started to shut down
// refuses the add, checked under the stripe lock that the shutdown's detach
// walk also takes. A failure releases the slot and frees reg directly, since
// nothing could have seen it. Raises reg_count on success.
ceventloop_reg ceventloop_add(ceventloop loop, cselectable sel,
                              ceventloop_handlers handlers, void *arg,
                              char **err_str) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) {
    if (err_str) *err_str = CCOL__ERR_STR("Invalid or stale ceventloop handle");
    return CEVENTLOOP_REG_NULL;
  }

  ccol_retval validate = ceventloop_validate_add_args(raw, &sel);
  if (validate != ccol_success) {
    if (err_str)
      *err_str = CCOL__ERR_STR("Invalid arguments to ceventloop_add");
    ceventloop_resolve_unpin(raw);
    return CEVENTLOOP_REG_NULL;
  }

  ccol_retval create_rv = ccol_success;
  ceventloop_reg_s *reg =
      ceventloop_reg_create(raw, sel, handlers, arg, &create_rv);
  if (!reg) {
    if (err_str)
      *err_str =
          create_rv == ccol_not_enough_memory
              ? CCOL__ERR_STR(
                    "ceventloop_add: failed to allocate the registration")
              : CCOL__ERR_STR(
                    "ceventloop_add: a system call failed (the "
                    "initialisation of a mutex or a condition variable)");
    ceventloop_resolve_unpin(raw);
    return CEVENTLOOP_REG_NULL;
  }

  ceventloop_reg h = ceventloop_reg_slot_acquire(raw, reg);
  if (h == 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR(
          "ceventloop_add: failed to allocate the registration handle "
          "slot");
    ceventloop_reg_free(raw, reg);
    ceventloop_resolve_unpin(raw);
    return CEVENTLOOP_REG_NULL;
  }

  size_t idx =
      (sel.type == cselectable_type_fd)
          ? stripe_index_for_fd(raw, sel.fd)
          : (atomic_fetch_add(&raw->next_queue_stripe, 1) % raw->num_stripes);
  ceventloop_stripe *stripe = &raw->stripes[idx];

  ccol__mutex_lock(stripe->lock);

  bool shut_down = atomic_load(&raw->shutting_down);
  ccol_retval rv;
  if (shut_down) {
    rv = ccol_not_permitted;
  } else if (sel.type == cselectable_type_fd) {
    rv = ceventloop_add_fd(raw, idx, reg);
  } else {
    event_entry *entry =
        ccol__mem_calloc(raw->m_procs, 1, ceventloop_entry_alloc_size(raw));
    if (!entry) {
      rv = ccol_not_enough_memory;
    } else {
      entry->is_fd = false;
      entry->fd = -1;
      entry->stripe_idx = idx;
      entry->as.reg = reg;
      if (ccol__mutex_init(entry->dispatch_lock) != 0) {
        ccol__mem_free(raw->m_procs, entry);
        rv = ccol_unexpected_failure;
      } else {
        atomic_init(&entry->removed, false);
        atomic_init(&entry->refcount, (size_t)0);
        entry->generation =
            atomic_fetch_add(&raw->fd_generation_counter, 1) + 1;
        rv = ceventloop_add_queue(raw, entry, reg);
        if (rv == ccol_success) {
          reg->owning_entry = entry;
          reg->stripe_idx = idx;
          reg->generation = entry->generation;
          loop_queue_list_add(stripe, reg);
        } else {
          ccol__mutex_destroy(entry->dispatch_lock);
          ccol__mem_free(raw->m_procs, entry);
        }
      }
    }
  }

  if (rv == ccol_success) atomic_fetch_add(&raw->reg_count, 1);

  ccol__mutex_unlock(stripe->lock);

  if (rv != ccol_success) {
    if (err_str) {
      if (shut_down)
        *err_str = CCOL__ERR_STR("ceventloop_add: the loop is shut down");
      else if (rv == ccol_not_permitted)
        *err_str = CCOL__ERR_STR(
            "ceventloop_add: direction already in use on this fd");
      else if (rv == ccol_not_enough_memory)
        *err_str = CCOL__ERR_STR(
            "ceventloop_add: failed to allocate the registration");
      else
        *err_str = CCOL__ERR_STR(
            "ceventloop_add: a system call failed (epoll_ctl, eventfd "
            "or a mutex initialisation)");
    }
    ceventloop_reg_slot_release(raw, reg);
    ceventloop_reg_free(raw, reg);
    ceventloop_resolve_unpin(raw);
    return CEVENTLOOP_REG_NULL;
  }

#ifdef RUNNING_UNIT_TESTS
  void (*add_hook)(void) = atomic_load(&g_evl_add_before_return_hook);
  if (add_hook) add_hook();
#endif
  ceventloop_resolve_unpin(raw);
  if (err_str) *err_str = NULL;
  return h;
}

// Gives the generation of the fd behind reg, for the caller's own bookkeeping
// across fd reuse, or 0 when the loop or reg doesn't resolve. It resolves and
// pins both handles for the read.
uint64_t ceventloop_reg_generation(ceventloop loop, ceventloop_reg reg) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return 0;
  ceventloop_reg_s *raw_reg = ceventloop_reg_resolve(raw, reg);
  if (!raw_reg) {
    ceventloop_resolve_unpin(raw);
    return 0;
  }
  uint64_t gen = raw_reg->generation;
  ceventloop_reg_resolve_unpin(raw, raw_reg);
  ceventloop_resolve_unpin(raw);
  return gen;
}

// Applies the poll interest an entry wants right now, computed from live state
// (each direction's registration, paused and muted included), never from a
// snapshot. Modify, a partial remove, pause, resume and the one-shot re-arm
// after a pool dispatch all use it, so whichever runs last under the stripe
// lock applies the current answer and none can replay a stale mask. When no fd
// direction wants real interest the fd is taken out with DEL (ERR and HUP can't
// be masked, and a paused fd in error would spin the poller) and added back
// with ADD later. A removed entry, whose fd may be closed or reused, and a
// queue entry whose registration is gone are left alone. The caller holds the
// stripe lock.
static void ceventloop_rearm_entry_locked(struct ceventloop__s *loop,
                                          event_entry *entry) {
  if (atomic_load(&entry->removed)) return;

  ccol_poll_event ev;
  ev.data.ptr = entry;

  if (entry->is_fd) {
    uint32_t mask = loop->dispatch_pool ? CCOL_POLL_ONESHOT : 0;
    mask |= fd_reg_interest_bits(entry->as.fd.read_reg, cselect_read);
    mask |= fd_reg_interest_bits(entry->as.fd.write_reg, cselect_write);

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

// Changes the direction of an fd registration, for a caller whose interest
// changes over time (a connecting socket that then reads). It resolves and pins
// the loop and the registration, then under the stripe lock checks that reg
// isn't removed (which makes owning_entry safe), moves reg to the other slot of
// its entry, clears muted and re-arms the entry. The same direction only
// unmutes and re-arms. Callbacks get a snapshot of reg->sel taken under the
// stripe lock, so the write here never races one. Gives ccol_success,
// ccol_invalid_args (bad handle, removed or queue registration, bad new_dir) or
// ccol_not_permitted when the other slot is taken.
ccol_retval ceventloop_modify(ceventloop loop, ceventloop_reg reg_h,
                              cselect_dir new_dir) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  if (new_dir != cselect_read && new_dir != cselect_write) {
    ceventloop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ceventloop_reg_s *reg = ceventloop_reg_resolve(raw, reg_h);
  if (!reg) {
    ceventloop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (reg->sel.type != cselectable_type_fd) {
    ceventloop_reg_resolve_unpin(raw, reg);
    ceventloop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ceventloop_stripe *stripe = &raw->stripes[reg->stripe_idx];
  ccol__mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    ccol__mutex_unlock(stripe->lock);
    ceventloop_reg_resolve_unpin(raw, reg);
    ceventloop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (reg->sel.dir == new_dir) {
    if (reg->muted) {
      reg->muted = false;
      ceventloop_rearm_entry_locked(raw, reg->owning_entry);
    }
    ccol__mutex_unlock(stripe->lock);
    ceventloop_reg_resolve_unpin(raw, reg);
    ceventloop_resolve_unpin(raw);
    return ccol_success;
  }

  event_entry *entry = reg->owning_entry;
  ceventloop_reg_s **target_slot = (new_dir == cselect_read)
                                       ? &entry->as.fd.read_reg
                                       : &entry->as.fd.write_reg;
  if (*target_slot != NULL) {
    ccol__mutex_unlock(stripe->lock);
    ceventloop_reg_resolve_unpin(raw, reg);
    ceventloop_resolve_unpin(raw);
    return ccol_not_permitted;
  }

  ceventloop_reg_s **current_slot = (reg->sel.dir == cselect_read)
                                        ? &entry->as.fd.read_reg
                                        : &entry->as.fd.write_reg;
  *current_slot = NULL;
  *target_slot = reg;
  reg->sel.dir = new_dir;
  reg->muted = false;

  ceventloop_rearm_entry_locked(raw, entry);

  ccol__mutex_unlock(stripe->lock);
  ceventloop_reg_resolve_unpin(raw, reg);
  ceventloop_resolve_unpin(raw);
  return ccol_success;
}

// Shared checks of pause and resume: reg must be an fd registration that isn't
// removed (confirmed under the stripe lock before owning_entry is read; a
// queue's bridge eventfd has no use for a pause). Gives ccol_success and the
// entry in *out_entry, or ccol_invalid_args.
static ccol_retval ceventloop_pause_resume_validate_locked(
    struct ceventloop__s *loop, ceventloop_reg_s *reg,
    event_entry **out_entry) {
  (void)loop;
  if (reg->sel.type != cselectable_type_fd) return ccol_invalid_args;
  if (atomic_load(&reg->removed)) return ccol_invalid_args;
  *out_entry = reg->owning_entry;
  return ccol_success;
}

// Stops event delivery for an fd registration while keeping it whole, the
// cheap form of remove plus add for a connection that leaves the reactor for a
// while. Under the stripe lock it sets paused and re-arms the entry without it.
// A paused registration stays paused and succeeds. Gives ccol_success or
// ccol_invalid_args.
ccol_retval ceventloop_pause(ceventloop loop, ceventloop_reg reg_h) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return ccol_invalid_args;

  ceventloop_reg_s *reg = ceventloop_reg_resolve(raw, reg_h);
  if (!reg) {
    ceventloop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ceventloop_stripe *stripe = &raw->stripes[reg->stripe_idx];
  ccol__mutex_lock(stripe->lock);

  event_entry *entry = NULL;
  ccol_retval rv = ceventloop_pause_resume_validate_locked(raw, reg, &entry);
  if (rv != ccol_success) {
    ccol__mutex_unlock(stripe->lock);
    ceventloop_reg_resolve_unpin(raw, reg);
    ceventloop_resolve_unpin(raw);
    return rv;
  }

  if (!atomic_load(&reg->paused)) {
    atomic_store(&reg->paused, true);
    ceventloop_rearm_entry_locked(raw, entry);
  }

  ccol__mutex_unlock(stripe->lock);
  ceventloop_reg_resolve_unpin(raw, reg);
  ceventloop_resolve_unpin(raw);
  return ccol_success;
}

// Starts event delivery again for a paused (or muted) fd registration, by
// clearing both flags under the stripe lock and re-arming the entry. Resuming
// anything else succeeds and does nothing, so a caller racing its own pause and
// resume against a remove needn't tell the cases apart. Gives ccol_success or
// ccol_invalid_args.
ccol_retval ceventloop_resume(ceventloop loop, ceventloop_reg reg_h) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return ccol_invalid_args;

  ceventloop_reg_s *reg = ceventloop_reg_resolve(raw, reg_h);
  if (!reg) {
    ceventloop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ceventloop_stripe *stripe = &raw->stripes[reg->stripe_idx];
  ccol__mutex_lock(stripe->lock);

  event_entry *entry = NULL;
  ccol_retval rv = ceventloop_pause_resume_validate_locked(raw, reg, &entry);
  if (rv != ccol_success) {
    ccol__mutex_unlock(stripe->lock);
    ceventloop_reg_resolve_unpin(raw, reg);
    ceventloop_resolve_unpin(raw);
    return rv;
  }

  if (atomic_load(&reg->paused) || reg->muted) {
    atomic_store(&reg->paused, false);
    reg->muted = false;
    ceventloop_rearm_entry_locked(raw, entry);
  }

  ccol__mutex_unlock(stripe->lock);
  ceventloop_reg_resolve_unpin(raw, reg);
  ceventloop_resolve_unpin(raw);
  return ccol_success;
}

// Pushes a retired entry onto the loop's lock-free list of entries waiting for
// the reclaim. Entries come from every stripe, so no stripe lock can guard the
// list. There's no ABA problem, since the reclaim only ever takes the whole
// list at once.
static void ceventloop_push_entry_free_node(struct ceventloop__s *loop,
                                            event_entry *entry) {
  event_entry *old_head = atomic_load(&loop->pending_entry_frees);
  do {
    entry->pending_free_next = old_head;
  } while (!atomic_compare_exchange_weak(&loop->pending_entry_frees, &old_head,
                                         entry));
}

// Retires an entry that ceventloop_remove fully unregistered: Marks it removed,
// stamps defer_gen and queues it for the reclaim. It can't be freed at once,
// because a batch the poller already fetched, or a running pool job, can still
// point at it.
static void ceventloop_defer_entry_free(struct ceventloop__s *loop,
                                        event_entry *entry) {
  atomic_store(&entry->removed, true);
  entry->defer_gen = atomic_load(&loop->poller_batch_gen);
  ceventloop_push_entry_free_node(loop, entry);
}

// Pushes a registration onto the loop's lock-free list waiting for the reclaim.
static void ceventloop_push_reg_free_node(struct ceventloop__s *loop,
                                          ceventloop_reg_s *reg) {
  ceventloop_reg_s *old_head = atomic_load(&loop->pending_reg_frees);
  do {
    reg->pending_free_next = old_head;
  } while (
      !atomic_compare_exchange_weak(&loop->pending_reg_frees, &old_head, reg));
}

// Queues a registration whose refcount reached 0 for the reclaim, which waits
// until no public call still holds a resolve pin on it.
static void ceventloop_defer_reg_free(struct ceventloop__s *loop,
                                      ceventloop_reg_s *reg) {
  ceventloop_push_reg_free_node(loop, reg);
}

// Frees every deferred entry and registration that is safe to free and keeps
// the rest. The poller calls it between batches after it advanced
// poller_batch_gen. An entry is safe once defer_gen < poller_batch_gen (no
// batch in flight can name it) and its refcount is 0 (no pool job uses it); a
// registration once its pending_resolve_count is 0, since its slot was released
// at the remove. Each list is taken whole with one exchange and the rest pushed
// back without a new stamp, so a busy loop can't starve an item. Gives true
// when something stays pending, which makes the poller use a bounded wait: The
// ping from the last unpin may not come.
static bool ceventloop_reclaim_pending_frees(struct ceventloop__s *loop) {
  uint64_t reached = atomic_load(&loop->poller_batch_gen);

  event_entry *e = atomic_exchange(&loop->pending_entry_frees, NULL);
  event_entry *e_keep = NULL;
  while (e) {
    event_entry *next = e->pending_free_next;
    if (e->defer_gen < reached && atomic_load(&e->refcount) == 0) {
      ccol__mutex_destroy(e->dispatch_lock);
      ccol__mem_free(loop->m_procs, e);
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
    ceventloop_push_entry_free_node(loop, e_keep);
    e_keep = next;
  }

  ceventloop_reg_s *r = atomic_exchange(&loop->pending_reg_frees, NULL);
  ceventloop_reg_s *r_keep = NULL;
  while (r) {
    ceventloop_reg_s *next = r->pending_free_next;
    if (atomic_load(&r->pending_resolve_count) == 0) {
      ceventloop_reg_free(loop, r);
    } else {
      r->pending_free_next = r_keep;
      r_keep = r;
    }
    r = next;
  }
  bool regs_still_pending = (r_keep != NULL);
  while (r_keep) {
    ceventloop_reg_s *next = r_keep->pending_free_next;
    ceventloop_push_reg_free_node(loop, r_keep);
    r_keep = next;
  }

  return entries_still_pending || regs_still_pending;
}

// Frees every deferred entry and registration with no check, for the destroy
// once every loop thread is joined and nothing can point at them.
static void ceventloop_free_all_pending(struct ceventloop__s *loop) {
  event_entry *e = atomic_exchange(&loop->pending_entry_frees, NULL);
  while (e) {
    event_entry *next = e->pending_free_next;
    ccol__mutex_destroy(e->dispatch_lock);
    ccol__mem_free(loop->m_procs, e);
    e = next;
  }
  ceventloop_reg_s *r = atomic_exchange(&loop->pending_reg_frees, NULL);
  while (r) {
    ceventloop_reg_s *next = r->pending_free_next;
    ceventloop_reg_free(loop, r);
    r = next;
  }
}

// Phase 1 of a remove: Unlinks reg from the registry and marks it removed,
// under the stripe lock, without touching the refcount. For an fd it clears
// reg's slot and either retires the entry (DEL when still added, out of the
// index, deferred) or re-arms it for the other direction. For a queue the
// splice of the waiter node, the removed store and the hand-on of a pending
// wake happen in one critical section of the queue's mutex: Split in two, a
// cascade step could read a frozen next pointer of a node whose successor
// another remove already reclaimed. A node a shutdown already detached only
// lowers the queue's detached count. Then the bridge eventfd leaves the set and
// entry->as.reg becomes NULL before the entry is deferred, so a stale batch
// reads NULL. Gives true when this call did the removal, false when an earlier
// one had.
static bool ceventloop_remove_unlink(struct ceventloop__s *raw,
                                     ceventloop_reg_s *reg) {
  ceventloop_stripe *stripe = &raw->stripes[reg->stripe_idx];
  ccol__mutex_lock(stripe->lock);

  if (atomic_load(&reg->removed)) {
    ccol__mutex_unlock(stripe->lock);
    return false;
  }

  event_entry *entry = reg->owning_entry;

  if (reg->sel.type == cselectable_type_fd) {
    ceventloop_reg_s **slot = (reg->sel.dir == cselect_read)
                                  ? &entry->as.fd.read_reg
                                  : &entry->as.fd.write_reg;
    *slot = NULL;
    if (entry->as.fd.read_reg == NULL && entry->as.fd.write_reg == NULL) {
      if (entry->epoll_added)
        ccol_poll_ctl(raw->epfd, CCOL_POLL_CTL_DEL, entry->fd, NULL,
                      &entry->poll_info);
      fd_registry_remove(stripe, entry->fd);
      ceventloop_defer_entry_free(raw, entry);
    } else {
      ceventloop_rearm_entry_locked(raw, entry);
    }
  } else {
    ccol__mutex *q_mtx;
    ccol_sel_waiter **q_head;
    ccol_sel_waiter **q_rotor;
    queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);

    ccol__mutex_lock(*q_mtx);
    bool linked = !reg->waiter_detached;
    if (linked)
      sel_unlink_waiter_locked(&reg->waiter_node, q_head, q_rotor);
    else
      (*queue_sel_detached_count(&reg->sel))--;
    atomic_store(&reg->removed, true);
    if (linked && queue_sel_is_ready(&reg->sel))
      notify_one_sel_waiter(q_head, q_rotor);
    ccol__mutex_unlock(*q_mtx);

    ccol_poll_ctl(raw->epfd, CCOL_POLL_CTL_DEL, reg->bridge_efd, NULL, NULL);
    loop_queue_list_remove(stripe, reg);
    entry->as.reg = NULL;
    ceventloop_defer_entry_free(raw, entry);
  }

  if (reg->sel.type == cselectable_type_fd) {
    atomic_store(&reg->removed, true);
  }

  ccol__mutex_unlock(stripe->lock);
  return true;
}

// Phase 2 of a remove, after the slot release: Lowers reg_count, drops the
// registration's own reference (deferring it when that was the last), and
// always pings the poller. Only the poller reclaims, and nothing else wakes it
// on an idle loop, so without the ping deferred entries would keep bridge
// eventfds open and an add and remove cycle would leak descriptors.
static void ceventloop_remove_finish(struct ceventloop__s *raw,
                                     ceventloop_reg_s *reg) {
  atomic_fetch_sub(&raw->reg_count, 1);

  int prev = atomic_fetch_sub(&reg->refcount, 1);
  if (prev == 1) ceventloop_defer_reg_free(raw, reg);

  eventfd_notify(raw->reclaim_wake_efd);
}

// Removes a registration. It resolves and pins the loop and the registration,
// unlinks it, releases its handle slot (before the registration can be
// reclaimed, so no resolve can find a reclaimable reg) and finishes the remove;
// a call that finds it already removed does neither. The pin it still holds
// keeps reg from being freed until the unpin. Gives ccol_success, or
// ccol_invalid_args when a handle doesn't resolve.
ccol_retval ceventloop_remove(ceventloop loop, ceventloop_reg reg_h) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  ceventloop_reg_s *reg = ceventloop_reg_resolve(raw, reg_h);
  if (!reg) {
    ceventloop_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  if (ceventloop_remove_unlink(raw, reg)) {
    ceventloop_reg_slot_release(raw, reg);
    ceventloop_remove_finish(raw, reg);
  }

  ceventloop_reg_resolve_unpin(raw, reg);
  ceventloop_resolve_unpin(raw);
  return ccol_success;
}

// Gives the number of live registrations, or ccol_invalid_size for a bad
// handle.
size_t ceventloop_reg_count(ceventloop loop) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return ccol_invalid_size;
  size_t n = atomic_load(&raw->reg_count);
  ceventloop_resolve_unpin(raw);
  return n;
}

// One registration collected for dispatch under the stripe lock and acted on
// after it. sel_snapshot is a copy of reg->sel taken under that lock, so a
// callback never reads reg->sel while ceventloop_modify writes it.
// queue_count_before and wake_hops, for queue items, are the message count and
// hop count taken when the wake was consumed, for the cascade step.
typedef struct dispatch_item {
  ceventloop_reg_s *reg;
  bool is_error;
  bool is_readable;
  bool is_writable;
  cselectable sel_snapshot;
  size_t queue_count_before;
  uint32_t wake_hops;
} dispatch_item;

static void ccol_fd_reg_mute_locked(struct ceventloop__s *loop,
                                    event_entry *entry, ceventloop_reg_s *reg);

// What one fd event means for one registration, decided the same way on both
// dispatch paths.
typedef enum {
  FD_TAKE_NONE,
  FD_TAKE_DIRECTION,
  FD_TAKE_ERROR,
} fd_take;

static fd_take ccol_fd_reg_takes_condition(struct ceventloop__s *loop,
                                           event_entry *entry,
                                           ceventloop_reg_s *reg,
                                           bool is_write_direction);

// Decides whether an event on one direction of an fd entry goes to reg, and to
// which handler, so both dispatch paths route events the same way. A removed
// registration gets nothing. A ready direction with its own handler takes the
// event even when the fd also has an error, so data that arrived first isn't
// lost; anything else goes to ccol_fd_reg_takes_condition(). The caller holds
// the stripe lock.
static inline fd_take ccol_fd_reg_takes_event(struct ceventloop__s *loop,
                                              event_entry *entry,
                                              ceventloop_reg_s *reg,
                                              bool is_err, bool direction_ready,
                                              bool is_write_direction) {
  if (!reg || atomic_load(&reg->removed) || !(is_err || direction_ready))
    return FD_TAKE_NONE;
  if (direction_ready &&
      (is_write_direction ? reg->handlers.on_writable != NULL
                          : reg->handlers.on_readable != NULL))
    return FD_TAKE_DIRECTION;
  return ccol_fd_reg_takes_condition(loop, entry, reg, is_write_direction);
}

// The rest of the routing rule, for an error, a hang-up or a ready direction
// without a handler: on_error when there is one, else the direction's handler
// (its own read or write reports the condition and clears a socket error, as in
// libevent and libuv). When neither exists reg is muted, since the condition
// may stay true and repeat on every wait. Out of line and cold, so plain
// readiness keeps the short inlined path. The caller holds the stripe lock.
static __attribute__((noinline, cold)) fd_take
ccol_fd_reg_takes_condition(struct ceventloop__s *loop, event_entry *entry,
                            ceventloop_reg_s *reg, bool is_write_direction) {
  if (reg->handlers.on_error) return FD_TAKE_ERROR;
  if (is_write_direction ? reg->handlers.on_writable != NULL
                         : reg->handlers.on_readable != NULL)
    return FD_TAKE_DIRECTION;
  ccol_fd_reg_mute_locked(loop, entry, reg);
  return FD_TAKE_NONE;
}

// Mutes reg and re-arms its entry from live state, which takes reg out of the
// mask while a sibling direction with handlers stays armed. A pool event that
// arrives before the sibling's job finishes is dropped by the in-flight check,
// and the job's re-arm applies the live state again. The caller holds the
// stripe lock.
static __attribute__((noinline, cold)) void ccol_fd_reg_mute_locked(
    struct ceventloop__s *loop, event_entry *entry, ceventloop_reg_s *reg) {
  reg->muted = true;
  ceventloop_rearm_entry_locked(loop, entry);
}

// Fills a dispatch item for one direction of an fd entry from the routing
// decision take, so both dispatch paths build items the same way. A ready
// direction with a handler gives a readable or writable item; anything else is
// an error item. With fd_reg_interest_bits() never arming IN for a registration
// without a reader, IN there means the peer hung up. The selectable is copied
// into the item.
static inline void ccol_fd_item_fill(dispatch_item *item, ceventloop_reg_s *reg,
                                     fd_take take, bool is_write_direction) {
  bool dispatch_on_direction = (take == FD_TAKE_DIRECTION);
  item->reg = reg;
  item->is_readable = !is_write_direction && dispatch_on_direction;
  item->is_writable = is_write_direction && dispatch_on_direction;
  item->is_error = !dispatch_on_direction;
  item->sel_snapshot = reg->sel;
}

// Calls the callback an item asks for, with the registration's own handle.
// Both dispatch paths use it. It checks removed and paused again right before
// the call, because the collection check is not enough: A remove or pause can
// finish between collection and this call (a long wait on the pool path), and
// the remover may already have freed arg.
static void ceventloop_run_callback(ceventloop loop, dispatch_item *item) {
  ceventloop_reg_s *reg = item->reg;
  if (atomic_load(&reg->removed)) return;
  if (atomic_load(&reg->paused)) return;
  if (item->is_error) {
    if (reg->handlers.on_error)
      reg->handlers.on_error(loop, reg->self_handle, &item->sel_snapshot,
                             reg->arg);
  } else if (item->is_readable) {
    if (reg->handlers.on_readable)
      reg->handlers.on_readable(loop, reg->self_handle, &item->sel_snapshot,
                                reg->arg);
  } else if (item->is_writable) {
    if (reg->handlers.on_writable)
      reg->handlers.on_writable(loop, reg->self_handle, &item->sel_snapshot,
                                reg->arg);
  }
}

// Drops the reference a collection took on reg. When it was the last one and
// reg is removed, it defers reg and pings the poller, since on a worker this
// may be the call that makes reg reclaimable and the poller wouldn't know.
// Nothing is read off reg after the defer: No resolve pin holds it, so the
// poller may free it at once.
static void ceventloop_release_after_dispatch(struct ceventloop__s *loop,
                                              ceventloop_reg_s *reg) {
  int prev = atomic_fetch_sub(&reg->refcount, 1);
  if (prev == 1 && atomic_load(&reg->removed)) {
    ceventloop_defer_reg_free(loop, reg);
    eventfd_notify(loop->reclaim_wake_efd);
  }
}

// Runs a queue item's callback with the thread marked as dispatching that
// queue, so a destroy of the queue from the callback is a fatal error instead
// of waiting for itself. The mark is cleared afterwards.
static void queue_dispatch_run_callback(struct ceventloop__s *loop,
                                        dispatch_item *item) {
  bool fell_back = queue_dispatch_mark(queue_sel_queue(&item->sel_snapshot));
  ceventloop_run_callback(loop->self_handle, item);
  queue_dispatch_unmark(fell_back);
}

// Dispatches one event on a loop with a single reactor thread, where the
// poller collects and runs callbacks itself. A NULL entry is the shutdown
// eventfd, which is never drained so every wait keeps seeing it. Otherwise it
// holds the entry's dispatch lock for the whole dispatch and the stripe lock
// only for the collection: It refines an fd event (safe there, since the fd is
// open while the entry isn't removed), routes it per direction through the
// shared rule, or consumes a queue registration's wake and claims its dispatch
// reference. Each collected registration gets a refcount; callbacks run with no
// stripe lock held, and a queue callback is followed by the cascade step and
// the release of the queue reference.
static void ceventloop_handle_event(struct ceventloop__s *loop,
                                    ccol_poll_event *ev) {
  event_entry *entry = (event_entry *)ev->data.ptr;

  if (entry == NULL) {
    return;
  }

  ccol__mutex_lock(entry->dispatch_lock);

  dispatch_item items[2];
  size_t n_items = 0;

  ceventloop_stripe *stripe = &loop->stripes[entry->stripe_idx];
  ccol__mutex_lock(stripe->lock);
  if (entry->is_fd && !atomic_load(&entry->removed))
    ccol_poll_refine(
        ev, entry->fd,
        fd_reg_interest_bits(entry->as.fd.read_reg, cselect_read) |
            fd_reg_interest_bits(entry->as.fd.write_reg, cselect_write),
        &entry->poll_info);

  if (entry->is_fd) {
    bool is_err = (ev->events & (CCOL_POLL_ERR | CCOL_POLL_HUP)) != 0;
    bool is_in = (ev->events & (CCOL_POLL_IN | CCOL_POLL_RDHUP)) != 0;
    bool is_out = (ev->events & CCOL_POLL_OUT) != 0;

    ceventloop_reg_s *r = entry->as.fd.read_reg;
    fd_take r_take =
        ccol_fd_reg_takes_event(loop, entry, r, is_err, is_in, false);
    if (r_take != FD_TAKE_NONE) {
      atomic_fetch_add(&r->refcount, 1);
      ccol_fd_item_fill(&items[n_items], r, r_take, false);
      n_items++;
    }
    ceventloop_reg_s *w = entry->as.fd.write_reg;
    fd_take w_take =
        ccol_fd_reg_takes_event(loop, entry, w, is_err, is_out, true);
    if (w_take != FD_TAKE_NONE) {
      atomic_fetch_add(&w->refcount, 1);
      ccol_fd_item_fill(&items[n_items], w, w_take, true);
      n_items++;
    }
  } else {
    ceventloop_reg_s *r = entry->as.reg;
    if (r && atomic_load(&r->removed)) {
      queue_reg_consume_wake(r, NULL, NULL);
    } else if (r) {
      queue_reg_consume_wake(r, &items[n_items].queue_count_before,
                             &items[n_items].wake_hops);
      atomic_fetch_add(&r->refcount, 1);
      queue_dispatch_ref_acquire(&r->sel);
      items[n_items].reg = r;
      items[n_items].is_error = false;
      items[n_items].is_readable = (r->sel.dir == cselect_read);
      items[n_items].is_writable = (r->sel.dir == cselect_write);
      items[n_items].sel_snapshot = r->sel;
      n_items++;
    }
  }

  ccol__mutex_unlock(stripe->lock);

  for (size_t i = 0; i < n_items; i++) {
    if (entry->is_fd) {
      ceventloop_run_callback(loop->self_handle, &items[i]);
    } else {
      queue_dispatch_run_callback(loop, &items[i]);
      ceventloop_queue_cascade_notify_next(
          loop, items[i].reg, items[i].sel_snapshot.dir,
          items[i].queue_count_before, items[i].wake_hops);
      queue_dispatch_ref_release(&items[i].sel_snapshot);
    }
    ceventloop_release_after_dispatch(loop, items[i].reg);
  }

  ccol__mutex_unlock(entry->dispatch_lock);
}

// One collected batch of up to two items for one entry, run as one pool job
// under one hold of the entry's dispatch lock, with the ordering of the single
// thread path. Each entry owns exactly one job as the tail of its allocation;
// the poller drops events for an entry with a job in flight, so a dispatch
// allocates nothing.
typedef struct dispatch_job {
  struct ceventloop__s *loop;
  event_entry *entry;
  dispatch_item items[2];
  size_t n_items;
} dispatch_job;

// The layout of one entry allocation in a loop with a dispatch pool.
typedef struct event_entry_with_job {
  event_entry entry;
  dispatch_job job;
} event_entry_with_job;

// Gives the size of one entry allocation of loop: With a dispatch pool the
// entry carries its job.
static size_t ceventloop_entry_alloc_size(const struct ceventloop__s *loop) {
  return loop->dispatch_pool ? sizeof(event_entry_with_job)
                             : sizeof(event_entry);
}

// Gives the job storage of an entry of a loop with a dispatch pool, which the
// poller fills and a worker runs.
static inline dispatch_job *ceventloop_entry_job(event_entry *entry) {
  return &((event_entry_with_job *)entry)->job;
}

static void ceventloop_dispatch_job_fn(void *arg);

// The thread-specific key that marks a loop's poller (for its whole life) and
// a dispatch worker running one of its jobs. Only the self-call guards of
// shutdown and destroy read it. One key covers every loop, since a pool's
// workers belong to one loop. live and the deletion work as for the queue
// dispatch marker; ceventloop_create_mp refuses to build a loop while live is
// false.
static struct {
  ccol__thread_ls_key key;
  ccol__once_flag once;
  atomic_bool live;
} ceventloop_job_key_bundle = {.once = CCOL__ONCE_INIT};

// Creates the job key once and records whether that worked.
static void ceventloop_init_job_key(void) {
  if (ccol__thread_ls_key_create(ceventloop_job_key_bundle.key, NULL) == 0)
    atomic_store(&ceventloop_job_key_bundle.live, true);
}

// The job mark of a thread whose key store failed, as
// queue_dispatch_mark_fallback.
static __thread const void *ceventloop_job_mark_fallback;

// Marks the calling thread as loop's poller or one of its dispatch workers, for
// the self-call guards. Gives true when the fallback copy holds the mark; pass
// that to ceventloop_job_unmark().
static bool ceventloop_job_mark(struct ceventloop__s *loop) {
  if (cthreadcomm_mark_set(ceventloop_job_key_bundle.key, loop) == 0)
    return false;
  ceventloop_job_mark_fallback = loop;
  return true;
}

// Clears what ceventloop_job_mark() set, when a job or the poller thread ends,
// so no later work on the thread carries the mark.
static void ceventloop_job_unmark(bool fell_back) {
  if (fell_back)
    ceventloop_job_mark_fallback = NULL;
  else
    (void)ccol__thread_ls_set(ceventloop_job_key_bundle.key, NULL);
}

// Says whether the calling thread is loop's poller or runs one of its dispatch
// jobs. The self-call guards of shutdown and destroy ask it, because such a
// call would join or free the thread it runs on.
static bool ceventloop_job_marked(const struct ceventloop__s *loop) {
  ccol__call_once(ceventloop_job_key_bundle.once, ceventloop_init_job_key);
  return ccol__thread_ls_get(ceventloop_job_key_bundle.key) ==
             (const void *)loop ||
         ceventloop_job_mark_fallback == (const void *)loop;
}

// The poller's half of a dispatch on a loop with a dispatch pool: Collects an
// entry's items under the stripe lock (no dispatch lock; there's one poller)
// and submits the entry's own job to the pool. An event for an entry whose job
// is still queued or running is dropped, because a callback that calls modify
// can re-arm the fd before it frees state that a second job would read; the
// running job's own re-arm reflects the live state. A queue's wake isn't
// consumed here and no queue reference is claimed; the job does both when it
// runs. On a successful collect the entry's refcount goes up. A failed submit
// releases the item references, re-arms the entry (the one-shot event is spent)
// and gives true, so the poller backs off.
static bool ceventloop_poller_collect(struct ceventloop__s *loop,
                                      ccol_poll_event *ev) {
  event_entry *entry = (event_entry *)ev->data.ptr;
  if (entry == NULL) return false;

  ceventloop_stripe *stripe = &loop->stripes[entry->stripe_idx];
  ccol__mutex_lock(stripe->lock);
  if (entry->is_fd && !atomic_load(&entry->removed))
    ccol_poll_refine(
        ev, entry->fd,
        fd_reg_interest_bits(entry->as.fd.read_reg, cselect_read) |
            fd_reg_interest_bits(entry->as.fd.write_reg, cselect_write),
        &entry->poll_info);

  if (atomic_load(&entry->refcount) > 0) {
    ccol__mutex_unlock(stripe->lock);
    return false;
  }

  dispatch_job *job = ceventloop_entry_job(entry);
  job->loop = loop;
  job->entry = entry;
  job->n_items = 0;

  if (entry->is_fd) {
    bool is_err = (ev->events & (CCOL_POLL_ERR | CCOL_POLL_HUP)) != 0;
    bool is_in = (ev->events & (CCOL_POLL_IN | CCOL_POLL_RDHUP)) != 0;
    bool is_out = (ev->events & CCOL_POLL_OUT) != 0;

    ceventloop_reg_s *r = entry->as.fd.read_reg;
    fd_take r_take =
        ccol_fd_reg_takes_event(loop, entry, r, is_err, is_in, false);
    if (r_take != FD_TAKE_NONE) {
      atomic_fetch_add(&r->refcount, 1);
      ccol_fd_item_fill(&job->items[job->n_items], r, r_take, false);
      job->n_items++;
    }
    ceventloop_reg_s *w = entry->as.fd.write_reg;
    fd_take w_take =
        ccol_fd_reg_takes_event(loop, entry, w, is_err, is_out, true);
    if (w_take != FD_TAKE_NONE) {
      atomic_fetch_add(&w->refcount, 1);
      ccol_fd_item_fill(&job->items[job->n_items], w, w_take, true);
      job->n_items++;
    }
  } else {
    ceventloop_reg_s *r = entry->as.reg;
    if (r && !atomic_load(&r->removed)) {
      atomic_fetch_add(&r->refcount, 1);
      dispatch_item *item = &job->items[job->n_items];
      item->reg = r;
      item->is_error = false;
      item->is_readable = (r->sel.dir == cselect_read);
      item->is_writable = (r->sel.dir == cselect_write);
      item->sel_snapshot = r->sel;
      job->n_items++;
    }
  }

  if (job->n_items == 0) {
    ccol__mutex_unlock(stripe->lock);
    return false;
  }

  atomic_fetch_add(&entry->refcount, 1);
  ccol__mutex_unlock(stripe->lock);

  ccol_retval rv = cthrpool_submit(loop->dispatch_pool,
                                   ceventloop_dispatch_job_fn, job, NULL);
  if (rv != ccol_success) {
    for (size_t i = 0; i < job->n_items; i++)
      ceventloop_release_after_dispatch(loop, job->items[i].reg);
    ccol__mutex_lock(stripe->lock);
    ceventloop_rearm_entry_locked(loop, entry);
    atomic_fetch_sub(&entry->refcount, 1);
    ccol__mutex_unlock(stripe->lock);
    return true;
  }
  return false;
}

// The pool task that runs one entry's job on a dispatch worker, marked as a
// worker of the loop. It holds the entry's dispatch lock across the callbacks.
// For a queue item it checks under the stripe lock that the registration isn't
// removed, and only then claims the queue reference and consumes the wake (the
// queue of a removed registration may be gone); after the callback come the
// cascade step and the release. Then it re-arms the entry and drops its
// refcount together under the stripe lock, re-arm first: A reclaim needs the
// refcount at 0, and the poller must never see the re-arm without the drop, or
// it would discard a new event that one-shot has already spent.
static void ceventloop_dispatch_job_fn(void *arg) {
  dispatch_job *job = (dispatch_job *)arg;
  struct ceventloop__s *loop = job->loop;
  event_entry *entry = job->entry;

  ccol__call_once(ceventloop_job_key_bundle.once, ceventloop_init_job_key);
  bool fell_back = ceventloop_job_mark(loop);

  ceventloop_stripe *stripe = &loop->stripes[entry->stripe_idx];

  ccol__mutex_lock(entry->dispatch_lock);
  for (size_t i = 0; i < job->n_items; i++) {
    dispatch_item *item = &job->items[i];
    if (entry->is_fd) {
      ceventloop_run_callback(loop->self_handle, item);
    } else {
      ccol__mutex_lock(stripe->lock);
      bool live = !atomic_load(&item->reg->removed);
      if (live) {
        queue_dispatch_ref_acquire(&item->sel_snapshot);
        queue_reg_consume_wake(item->reg, &item->queue_count_before,
                               &item->wake_hops);
      }
      ccol__mutex_unlock(stripe->lock);
      if (live) {
        queue_dispatch_run_callback(loop, item);
        ceventloop_queue_cascade_notify_next(
            loop, item->reg, item->sel_snapshot.dir, item->queue_count_before,
            item->wake_hops);
        queue_dispatch_ref_release(&item->sel_snapshot);
      }
    }
    ceventloop_release_after_dispatch(loop, item->reg);
  }
  ccol__mutex_unlock(entry->dispatch_lock);

  ccol__mutex_lock(stripe->lock);
  ceventloop_rearm_entry_locked(loop, entry);
  atomic_fetch_sub(&entry->refcount, 1);
  ccol__mutex_unlock(stripe->lock);

  ceventloop_job_unmark(fell_back);
}

// The poller's wait limit while a reclaim is pending: The guarantee that the
// reclaim runs, since a ping can be missed. 50 ms costs one wake and one list
// scan per interval, only while something is pending.
#define CEVENTLOOP_RECLAIM_RETRY_MS 50

// The first and the longest pause of ceventloop_dispatch_backoff().
#define CEVENTLOOP_DISPATCH_BACKOFF_MIN_MS 1
#define CEVENTLOOP_DISPATCH_BACKOFF_MAX_MS 64

// Pauses the poller after a batch with a failed submit, since the re-armed fd
// reports again at once and a lasting allocation failure would spin it. The
// pause in *backoff_ms doubles per failed batch up to the maximum, and the
// caller resets it after a good batch. It waits on shutdown_efd, so a shutdown
// ends it at once.
static __attribute__((noinline, cold)) void ceventloop_dispatch_backoff(
    struct ceventloop__s *loop, int *backoff_ms) {
  *backoff_ms =
      *backoff_ms == 0 ? CEVENTLOOP_DISPATCH_BACKOFF_MIN_MS : *backoff_ms * 2;
  if (*backoff_ms > CEVENTLOOP_DISPATCH_BACKOFF_MAX_MS)
    *backoff_ms = CEVENTLOOP_DISPATCH_BACKOFF_MAX_MS;
  struct pollfd pfd = {.fd = loop->shutdown_efd, .events = POLLIN};
  (void)poll(&pfd, 1, *backoff_ms);
}

// The poller thread of a loop. It's marked as the loop's poller for its whole
// life (callbacks and on_removed run here, and the guards go by that mark, not
// by thread ID, since a joined thread's ID gets reused). Each pass, until
// shutting_down, it bumps poller_batch_gen and reclaims what that makes safe,
// then waits, with CEVENTLOOP_RECLAIM_RETRY_MS as the limit while something is
// still pending. The reclaim ping is only drained. Every other event is
// dispatched inline with one thread or collected for the pool, and a batch with
// a failed submit backs off.
static void *ceventloop_thread_fn(void *arg) {
  struct ceventloop__s *loop = (struct ceventloop__s *)arg;
  ccol_poll_event *events = loop->poller_events;
  int backoff_ms = 0;

  bool fell_back = ceventloop_job_mark(loop);

  for (;;) {
    if (atomic_load(&loop->shutting_down)) break;

    atomic_fetch_add(&loop->poller_batch_gen, (uint64_t)1);
    bool reclaim_still_pending = ceventloop_reclaim_pending_frees(loop);

    int wait_timeout_ms =
        reclaim_still_pending ? CEVENTLOOP_RECLAIM_RETRY_MS : -1;
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
      if (events[i].data.ptr == &loop->reclaim_wake_efd) {
        eventfd_drain(loop->reclaim_wake_efd);
        continue;
      }
      if (loop->dispatch_pool) {
        dispatch_failed |= ceventloop_poller_collect(loop, &events[i]);
      } else {
        ceventloop_handle_event(loop, &events[i]);
      }
    }
    if (__builtin_expect(dispatch_failed, 0))
      ceventloop_dispatch_backoff(loop, &backoff_ms);
    else
      backoff_ms = 0;
  }

  ceventloop_job_unmark(fell_back);
  return NULL;
}

// Destroys the first created stripes (map and mutex) and frees the array, to
// roll back a failed ceventloop_create_mp. It frees through mmgmt_procs, since
// loop->m_procs may not be filled in yet.
static void destroy_stripes(struct ceventloop__s *loop,
                            ccol_memmgmt_procs *mmgmt_procs, size_t created) {
  for (size_t i = 0; i < created; i++) {
    chmap_destroy(loop->stripes[i].fd_index);
    ccol__mutex_destroy(loop->stripes[i].lock);
  }
  ccol__mem_free(mmgmt_procs, loop->stripes);
}

static void ceventloop_teardown_raw(struct ceventloop__s *loop);

// Creates a loop with its poll set, shutdown and reclaim eventfds, locks,
// registration table, stripes, the poller's event buffer, the dispatch pool
// (with more than one thread; its queue is unbounded so the poller never
// blocks) and the poller thread, and gives its handle, or CEVENTLOOP_NULL with
// *err_str set. max_events_per_wait must fit an int and its buffer size must
// not overflow, or the poller's first wait would fail and the handle would
// never dispatch. A process without the two thread-specific keys a dispatch
// sets gets no loop. Everything that can fail runs before the thread starts.
// The handle slot is taken last; when that fails the running threads are
// stopped through ceventloop_teardown_raw().
ceventloop ceventloop_create_mp(size_t max_events_per_wait,
                                size_t num_lock_stripes,
                                size_t num_reactor_threads,
                                ccol_memmgmt_procs *mmgmt_procs,
                                char **err_str) {
  if (max_events_per_wait == 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR("max_events_per_wait must be positive");
    return CEVENTLOOP_NULL;
  }
  if (max_events_per_wait > (size_t)INT_MAX) {
    if (err_str)
      *err_str = CCOL__ERR_STR("max_events_per_wait must not exceed INT_MAX");
    return CEVENTLOOP_NULL;
  }
  if (max_events_per_wait > SIZE_MAX / sizeof(ccol_poll_event)) {
    if (err_str)
      *err_str = CCOL__ERR_STR(
          "max_events_per_wait is too large: max_events_per_wait * "
          "sizeof(ccol_poll_event) would overflow size_t");
    return CEVENTLOOP_NULL;
  }
  if (num_lock_stripes == 0) {
    if (err_str) *err_str = CCOL__ERR_STR("num_lock_stripes must be positive");
    return CEVENTLOOP_NULL;
  }
  if (num_reactor_threads == 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR("num_reactor_threads must be positive");
    return CEVENTLOOP_NULL;
  }
  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err_str)) {
    return CEVENTLOOP_NULL;
  }

  ccol__call_once(ceventloop_job_key_bundle.once, ceventloop_init_job_key);
  if (!atomic_load(&ceventloop_job_key_bundle.live) ||
      !queue_dispatch_marker_available()) {
    if (err_str)
      *err_str =
          CCOL__ERR_STR("failed to create a thread-local key of ceventloop");
    return CEVENTLOOP_NULL;
  }

  struct ceventloop__s *loop = (struct ceventloop__s *)ccol__mem_alloc(
      mmgmt_procs, sizeof(struct ceventloop__s));
  if (!loop) {
    if (err_str)
      *err_str = CCOL__ERR_STR("Failed to allocate memory for ceventloop");
    return CEVENTLOOP_NULL;
  }

  if (!ccol__populate_mem_mgmt_procs(loop, mmgmt_procs, err_str)) {
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }

  loop->epfd = ccol_poll_create();
  if (loop->epfd < 0) {
    if (err_str) *err_str = CCOL__ERR_STR("epoll_create1 failed");
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }

  loop->shutdown_efd = ccol_wakefd_create();
  if (loop->shutdown_efd < 0) {
    if (err_str) *err_str = CCOL__ERR_STR("eventfd failed");
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }

  ccol_poll_event ev;
  ev.data.ptr = NULL;
  ev.events = CCOL_POLL_IN;
  if (ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_ADD, loop->shutdown_efd, &ev,
                    NULL) < 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR("epoll_ctl failed registering shutdown eventfd");
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }

  loop->reclaim_wake_efd = ccol_wakefd_create();
  if (loop->reclaim_wake_efd < 0) {
    if (err_str) *err_str = CCOL__ERR_STR("eventfd failed");
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }

  ev.data.ptr = &loop->reclaim_wake_efd;
  ev.events = CCOL_POLL_IN;
  if (ccol_poll_ctl(loop->epfd, CCOL_POLL_CTL_ADD, loop->reclaim_wake_efd, &ev,
                    NULL) < 0) {
    if (err_str)
      *err_str =
          CCOL__ERR_STR("epoll_ctl failed registering reclaim-wake eventfd");
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }

  if (ccol__mutex_init(loop->shutdown_lock) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("Failed to initialize shutdown_lock");
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }
  if (ccol__cond_var_init(loop->joined_cv) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("Failed to initialize joined_cv");
    ccol__mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }
  if (ccol__rw_lock_init(loop->reg_slot_rwlock) != 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR("Failed to initialize reg_slot_rwlock");
    ccol__cond_var_destroy(loop->joined_cv);
    ccol__mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }
  loop->reg_slots =
      cvector_create_full(sizeof(ceventloop_reg_slot), mmgmt_procs, NULL);
  if (!loop->reg_slots) {
    if (err_str) *err_str = CCOL__ERR_STR("Failed to allocate reg slot array");
    ccol__rw_lock_destroy(loop->reg_slot_rwlock);
    ccol__cond_var_destroy(loop->joined_cv);
    ccol__mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }
  loop->reg_lost_indices = 0;
  loop->reg_free_indices =
      cvector_create_full(sizeof(uint32_t), mmgmt_procs, NULL);
  if (!loop->reg_free_indices) {
    if (err_str)
      *err_str = CCOL__ERR_STR("Failed to allocate reg free-index array");
    cvector__destroy(loop->reg_slots);
    ccol__rw_lock_destroy(loop->reg_slot_rwlock);
    ccol__cond_var_destroy(loop->joined_cv);
    ccol__mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
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
  atomic_init(&loop->poller_iterations_for_tests, (uint64_t)0);
  atomic_init(&loop->entries_reclaimed_for_tests, (uint64_t)0);
#endif
  loop->num_stripes = num_lock_stripes;
  loop->num_reactor_threads = num_reactor_threads;
  loop->dispatch_pool = CTHRPOOL_NULL;

  loop->stripes = ccol__mem_calloc(mmgmt_procs, num_lock_stripes,
                                   sizeof(ceventloop_stripe));
  if (!loop->stripes) {
    if (err_str)
      *err_str = CCOL__ERR_STR("Failed to allocate lock stripe array");
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    ccol__rw_lock_destroy(loop->reg_slot_rwlock);
    ccol__cond_var_destroy(loop->joined_cv);
    ccol__mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }

  size_t stripes_created = 0;
  for (; stripes_created < num_lock_stripes; stripes_created++) {
    char *stripe_err = NULL;
    loop->stripes[stripes_created].fd_index = chashmap_create_full(
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_int, ccol_pointer,
        mmgmt_procs, NULL, NULL, &stripe_err);
    if (!loop->stripes[stripes_created].fd_index) {
      if (err_str)
        *err_str = stripe_err ? stripe_err
                              : CCOL__ERR_STR("Failed to create fd registry");
      destroy_stripes(loop, mmgmt_procs, stripes_created);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      ccol__rw_lock_destroy(loop->reg_slot_rwlock);
      ccol__cond_var_destroy(loop->joined_cv);
      ccol__mutex_destroy(loop->shutdown_lock);
      close(loop->reclaim_wake_efd);
      close(loop->shutdown_efd);
      close(loop->epfd);
      ccol__mem_free(mmgmt_procs, loop->m_procs);
      ccol__mem_free(mmgmt_procs, loop);
      return CEVENTLOOP_NULL;
    }
    if (ccol__mutex_init(loop->stripes[stripes_created].lock) != 0) {
      if (err_str) *err_str = CCOL__ERR_STR("Failed to initialize stripe lock");
      chmap_destroy(loop->stripes[stripes_created].fd_index);
      destroy_stripes(loop, mmgmt_procs, stripes_created);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      ccol__rw_lock_destroy(loop->reg_slot_rwlock);
      ccol__cond_var_destroy(loop->joined_cv);
      ccol__mutex_destroy(loop->shutdown_lock);
      close(loop->reclaim_wake_efd);
      close(loop->shutdown_efd);
      close(loop->epfd);
      ccol__mem_free(mmgmt_procs, loop->m_procs);
      ccol__mem_free(mmgmt_procs, loop);
      return CEVENTLOOP_NULL;
    }
    loop->stripes[stripes_created].queue_regs_head = NULL;
  }

  loop->poller_events = ccol__mem_alloc(
      mmgmt_procs, max_events_per_wait * sizeof(ccol_poll_event));
  if (!loop->poller_events) {
    if (err_str)
      *err_str = CCOL__ERR_STR("Failed to allocate the epoll_wait buffer");
    destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    ccol__rw_lock_destroy(loop->reg_slot_rwlock);
    ccol__cond_var_destroy(loop->joined_cv);
    ccol__mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }

  if (num_reactor_threads > 1) {
    char *pool_err = NULL;
    loop->dispatch_pool =
        cthrpool_create_mp(num_reactor_threads - 1, 0, mmgmt_procs, &pool_err);
    if (!loop->dispatch_pool) {
      if (err_str)
        *err_str = pool_err ? pool_err
                            : CCOL__ERR_STR("Failed to create dispatch pool");
      ccol__mem_free(mmgmt_procs, loop->poller_events);
      destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
      cvector_destroy(loop->reg_free_indices);
      cvector_destroy(loop->reg_slots);
      ccol__rw_lock_destroy(loop->reg_slot_rwlock);
      ccol__cond_var_destroy(loop->joined_cv);
      ccol__mutex_destroy(loop->shutdown_lock);
      close(loop->reclaim_wake_efd);
      close(loop->shutdown_efd);
      close(loop->epfd);
      ccol__mem_free(mmgmt_procs, loop->m_procs);
      ccol__mem_free(mmgmt_procs, loop);
      return CEVENTLOOP_NULL;
    }
  }

  if (ccol__thread_create(loop->poller_thread, ceventloop_thread_fn, loop) !=
      0) {
    if (err_str) *err_str = CCOL__ERR_STR("pthread_create failed");
    if (loop->dispatch_pool) cthrpool__destroy(loop->dispatch_pool);
    ccol__mem_free(mmgmt_procs, loop->poller_events);
    destroy_stripes(loop, mmgmt_procs, num_lock_stripes);
    cvector_destroy(loop->reg_free_indices);
    cvector_destroy(loop->reg_slots);
    ccol__rw_lock_destroy(loop->reg_slot_rwlock);
    ccol__cond_var_destroy(loop->joined_cv);
    ccol__mutex_destroy(loop->shutdown_lock);
    close(loop->reclaim_wake_efd);
    close(loop->shutdown_efd);
    close(loop->epfd);
    ccol__mem_free(mmgmt_procs, loop->m_procs);
    ccol__mem_free(mmgmt_procs, loop);
    return CEVENTLOOP_NULL;
  }

  atomic_init(&loop->pending_resolve_count, (size_t)0);
#if CCOL_FORK_SAFETY_REQUIRED
  atomic_init(&loop->foreign_since_fork, false);
#endif

  ceventloop h = ceventloop_handle_slot_acquire(loop);
  if (h == 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR("Failed to allocate ceventloop handle slot");
    ceventloop_teardown_raw(loop);
    return CEVENTLOOP_NULL;
  }
  loop->self_handle = h;

  if (err_str) *err_str = NULL;
  return h;
}

// Unlinks the waiter node of every queue registration of loop once nothing in
// it dispatches, so the shut-down loop doesn't keep taking wakes the queue's
// other listeners need. A wake moves on when the queue is ready, as on a
// remove, and the queue counts the registration as detached. Takes each stripe
// lock, then each queue's mutex.
static void ceventloop_detach_queue_regs(struct ceventloop__s *loop) {
  for (size_t i = 0; i < loop->num_stripes; i++) {
    ceventloop_stripe *stripe = &loop->stripes[i];
    ccol__mutex_lock(stripe->lock);
    for (ceventloop_reg_s *reg = stripe->queue_regs_head; reg;
         reg = reg->loop_list_next) {
      ccol__mutex *q_mtx;
      ccol_sel_waiter **q_head;
      ccol_sel_waiter **q_rotor;
      queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
      ccol__mutex_lock(*q_mtx);
      if (!reg->waiter_detached && !atomic_load(&reg->removed)) {
        sel_unlink_waiter_locked(&reg->waiter_node, q_head, q_rotor);
        reg->waiter_detached = true;
        (*queue_sel_detached_count(&reg->sel))++;
        if (queue_sel_is_ready(&reg->sel))
          notify_one_sel_waiter(q_head, q_rotor);
      }
      ccol__mutex_unlock(*q_mtx);
    }
    ccol__mutex_unlock(stripe->lock);
  }
}

// Stops a resolved loop: The shutdown and teardown paths use it. A call from
// the loop's poller or one of its dispatch workers gives ccol_not_permitted,
// since it would join itself. The first caller (under shutdown_lock) sets
// shutting_down, writes shutdown_efd, which nothing drains, so the poller
// always sees it, and joins the poller; an inherited loop skips both, since its
// poller doesn't exist here and the eventfd is the parent's. Then it drains the
// pool (no submit can come once the poller is joined), detaches the queue
// registrations and wakes the other callers, who wait on joined_cv.
static ccol_retval ceventloop_shutdown_internal(struct ceventloop__s *loop) {
  if (ceventloop_job_marked(loop)) return ccol_not_permitted;

  ccol__mutex_lock(loop->shutdown_lock);
  bool is_leader = !loop->shutdown_started;
  loop->shutdown_started = true;
  ccol__mutex_unlock(loop->shutdown_lock);

  if (is_leader) {
    atomic_store(&loop->shutting_down, true);

#if CCOL_FORK_SAFETY_REQUIRED
    bool is_foreign = atomic_load(&loop->foreign_since_fork);
#else
    bool is_foreign = false;
#endif
    if (is_foreign) {
    } else {
      eventfd_notify(loop->shutdown_efd);

      ccol__thread_join(loop->poller_thread);
    }

    if (loop->dispatch_pool) {
      cthrpool_shutdown_drain(loop->dispatch_pool);
    }

    if (!is_foreign) ceventloop_detach_queue_regs(loop);

    ccol__mutex_lock(loop->shutdown_lock);
    loop->joined = true;
    ccol__cond_var_broadcast(loop->joined_cv);
    ccol__mutex_unlock(loop->shutdown_lock);
  } else {
    ccol__mutex_lock(loop->shutdown_lock);
    while (!loop->joined) {
      ccol__cond_var_wait(loop->joined_cv, loop->shutdown_lock);
    }
    ccol__mutex_unlock(loop->shutdown_lock);
  }

  return ccol_success;
}

// Stops the loop's threads and drains its dispatches. It resolves and pins the
// loop for the call. Gives ccol_success, ccol_invalid_args for a bad handle, or
// ccol_not_permitted from one of the loop's own threads.
ccol_retval ceventloop_shutdown(ceventloop loop) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return ccol_invalid_args;
  ccol_retval rv = ceventloop_shutdown_internal(raw);
  ceventloop_resolve_unpin(raw);
  return rv;
}

// Shuts a loop down (if that hasn't happened) and frees everything it owns:
// the deferred items, every registration left in the stripes (each with its
// on_removed), the stripes, the pool, the tables, the locks and the
// descriptors. The destroy uses it after its pin wait, and the create's
// rollback; no caller can resolve the loop by then and every loop thread is
// joined, so no lock is needed. The fd index is walked without allocating, so a
// destroy short of memory still frees everything. A queue registration still
// linked to its queue is unlinked and hands on its wake, since the queue may
// have other listeners; a registration is freed before its entry, because the
// free reads owning_entry.
static void ceventloop_teardown_raw(struct ceventloop__s *loop) {
  ceventloop_shutdown_internal(loop);

  ceventloop_free_all_pending(loop);
  ccol__mem_free(loop->m_procs, loop->poller_events);

  for (size_t i = 0; i < loop->num_stripes; i++) {
    ceventloop_stripe *stripe = &loop->stripes[i];

    ccol_chmap_walk walk = {0};
    const void *key_bytes;
    const void *val_bytes;
    while (
        ccol_chmap_walk_next(stripe->fd_index, &walk, &key_bytes, &val_bytes)) {
      event_entry *entry;
      memcpy(&entry, val_bytes, sizeof(entry));
      if (entry->as.fd.read_reg)
        ceventloop_reg_free(loop, entry->as.fd.read_reg);
      if (entry->as.fd.write_reg)
        ceventloop_reg_free(loop, entry->as.fd.write_reg);
      ccol__mutex_destroy(entry->dispatch_lock);
      ccol__mem_free(loop->m_procs, entry);
    }
    chmap_destroy(stripe->fd_index);

    ceventloop_reg_s *reg = stripe->queue_regs_head;
    while (reg) {
      ceventloop_reg_s *next = reg->loop_list_next;
      ccol__mutex *q_mtx;
      ccol_sel_waiter **q_head;
      ccol_sel_waiter **q_rotor;
      queue_sel_locate(&reg->sel, &q_mtx, &q_head, &q_rotor);
      ccol__mutex_lock(*q_mtx);
      if (!reg->waiter_detached) {
        sel_unlink_waiter_locked(&reg->waiter_node, q_head, q_rotor);
        if (queue_sel_is_ready(&reg->sel))
          notify_one_sel_waiter(q_head, q_rotor);
      } else {
        (*queue_sel_detached_count(&reg->sel))--;
      }
      ccol__mutex_unlock(*q_mtx);
      event_entry *owning = reg->owning_entry;
      ceventloop_reg_free(loop, reg);
      ccol__mutex_destroy(owning->dispatch_lock);
      ccol__mem_free(loop->m_procs, owning);
      reg = next;
    }

    ccol__mutex_destroy(stripe->lock);
  }
  ccol__mem_free(loop->m_procs, loop->stripes);
  if (loop->dispatch_pool) cthrpool__destroy(loop->dispatch_pool);

  cvector_destroy(loop->reg_free_indices);
  cvector_destroy(loop->reg_slots);
  ccol__rw_lock_destroy(loop->reg_slot_rwlock);
  ccol__cond_var_destroy(loop->joined_cv);
  ccol__mutex_destroy(loop->shutdown_lock);
  close(loop->reclaim_wake_efd);
  close(loop->shutdown_efd);
  close(loop->epfd);

  if (loop->m_procs) {
    ccol_free free_func = loop->m_procs->free;
    free_func(loop->m_procs);
    free_func(loop);
  } else {
    ccol__default_mem_free(loop);
  }
}

// Destroys a loop. Under the write lock of the loop table it finds the slot and
// claims the teardown (destroy_claimed), so a second destroy, racing or later,
// or a stale handle, is a fatal error; so is a call from the loop's own thread,
// which would free the frame it runs in. The handle still resolves during the
// shutdown, so callbacks can remove their registrations while it drains. Then
// the slot stops resolving, it waits for every pin to drop, tears the loop
// down, and finally clears the slot, bumps its generation, frees the index and
// does a release of the table that the exit destructor deferred. The slot is
// always fetched by index again after an unlock, since a create can move the
// array.
void ceventloop__destroy(ceventloop loop) {
  if (!loop) return;

  ccol__call_once(ceventloop_slot_table.once,
                  ceventloop_slot_table_init_globals);
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
  uint32_t idx = (uint32_t)(loop >> 32);
  uint32_t gen = (uint32_t)(loop & 0xFFFFFFFFu);
  ccol__rw_lock_wrlock(ceventloop_slot_table.rwlock);
  struct ceventloop__s *raw = NULL;
  if (idx < event_loop_slot_count_locked()) {
    ceventloop_slot *s =
        (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, idx);
    if (s->in_use && s->generation == gen && !s->ptr->destroy_claimed)
      raw = s->ptr;
  }
  if (!raw) {
    ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
    ccol__fatal_err(
        "ceventloop_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a ceventloop handle)");
  }

  bool is_self_call = ceventloop_job_marked(raw);
  if (is_self_call) {
    ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
    ccol__fatal_err(
        "ceventloop_destroy: called from within a callback running on "
        "this "
        "loop's own reactor/dispatch thread (self-destroy hazard); defer "
        "destruction to another thread, or to after the callback returns, "
        "instead");
  }

  raw->destroy_claimed = true;
  ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);

  ceventloop_shutdown_internal(raw);

  ccol__rw_lock_wrlock(ceventloop_slot_table.rwlock);
  ((ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, idx))->in_use =
      false;
  ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);

  while (atomic_load(&raw->pending_resolve_count) > 0) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000};
    nanosleep(&ts, NULL);
  }

  ceventloop_teardown_raw(raw);

  ccol__rw_lock_wrlock(ceventloop_slot_table.rwlock);
  ceventloop_slot *slot2 =
      (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++;
  ceventloop_free_index_release_locked(idx);
  release_event_loop_slot_table_if_deferred_locked();
  ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
// Test-only: How many jobs wait in or run on the loop's dispatch pool.
size_t ceventloop__dispatch_pool_pending_count_for_tests(ceventloop loop) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw || !raw->dispatch_pool) {
    if (raw) ceventloop_resolve_unpin(raw);
    return 0;
  }
  size_t n = cthrpool_pending_count(raw->dispatch_pool);
  ceventloop_resolve_unpin(raw);
  return n;
}

// Test-only: The poller's thread ID, so a test can prove the guards don't
// recognise the poller by its ID.
ccol__thread_id ceventloop__poller_thread_for_tests(ceventloop loop) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  ccol__thread_id id;
  memset(&id, 0, sizeof(id));
  if (!raw) return id;
  id = raw->poller_thread;
  ceventloop_resolve_unpin(raw);
  return id;
}

// Test-only: Whether the calling thread carries loop's job mark.
bool ceventloop__test_runs_on_loop_thread(ceventloop loop) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return false;
  bool marked = ceventloop_job_marked(raw);
  ceventloop_resolve_unpin(raw);
  return marked;
}

// Test-only: How many waits the poller has finished, to spot a busy spin.
uint64_t ceventloop__poller_iterations_for_tests(ceventloop loop) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return 0;
  uint64_t n = atomic_load(&raw->poller_iterations_for_tests);
  ceventloop_resolve_unpin(raw);
  return n;
}

// Test-only: How many entries the reclaim has freed.
uint64_t ceventloop__entries_reclaimed_for_tests(ceventloop loop) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return 0;
  uint64_t n = atomic_load_explicit(&raw->entries_reclaimed_for_tests,
                                    memory_order_relaxed);
  ceventloop_resolve_unpin(raw);
  return n;
}

// Test-only: Sets poller_batch_gen, so a test can check a deferred free across
// a 32-bit boundary.
void ceventloop__set_poller_batch_gen_for_tests(ceventloop loop,
                                                uint64_t value) {
  struct ceventloop__s *raw = ceventloop_resolve(loop);
  if (!raw) return;
  atomic_store(&raw->poller_batch_gen, value);
  ceventloop_resolve_unpin(raw);
}

// Test-only: Resolves a handle without a pin, so a test has no unpin to forget
// (a forgotten one would hang every later destroy).
struct ceventloop__s *ceventloop__resolve_for_tests(ceventloop h) {
  ccol__call_once(ceventloop_slot_table.once,
                  ceventloop_slot_table_init_globals);
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol__rw_lock_rdlock(ceventloop_slot_table.rwlock);
  struct ceventloop__s *raw = NULL;
  if (idx < event_loop_slot_count_locked()) {
    ceventloop_slot *slot =
        (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
  return raw;
}

// Test-only: How many slots the loop table holds, freed ones included.
size_t ceventloop__slot_table_capacity_for_tests(void) {
  ccol__call_once(ceventloop_slot_table.once,
                  ceventloop_slot_table_init_globals);
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
  ccol__rw_lock_rdlock(ceventloop_slot_table.rwlock);
  size_t n = event_loop_slot_count_locked();
  ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
  return n;
}

// Test-only: Holds a real pin on h until the test sets *release, after setting
// *pinned, so a test can prove a destroy waits for pins.
bool ceventloop__resolve_pin_until_for_tests(ceventloop h, atomic_int *pinned,
                                             atomic_int *release) {
  struct ceventloop__s *raw = ceventloop_resolve(h);
  if (!raw) return false;
  atomic_store(pinned, 1);
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  while (!atomic_load(release)) nanosleep(&ts, NULL);
  ceventloop_resolve_unpin(raw);
  return true;
}

// Test-only: Write-locks and unlocks the loop table directly, so a thread other
// than the forking one can hold it across a fork(). The lock registers the fork
// handlers first, as every library path does.
void ceventloop__test_wrlock_slot_table_for_tests(void) {
  ccol__call_once(ceventloop_slot_table.once,
                  ceventloop_slot_table_init_globals);
  ccol__call_once(g_cthreadcomm_atfork_once, cthreadcomm_register_atfork_once);
  ccol__rw_lock_wrlock(ceventloop_slot_table.rwlock);
}

void ceventloop__test_wrunlock_slot_table_for_tests(void) {
  ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
}
#endif

// Says whether any slot still names a loop, for the exit destructor and the
// deferred release. It tests ptr, not in_use, since a destroy clears in_use
// before its teardown ends. The caller holds the write lock.
static bool event_loop_any_slot_live_locked(void) {
  size_t slot_count = event_loop_slot_count_locked();
  for (size_t i = 0; i < slot_count; i++) {
    ceventloop_slot *slot =
        (ceventloop_slot *)cvector_at(ceventloop_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

// Frees the table's vectors (setting them to NULL, so a later call sees the
// release) and deletes the two thread-specific keys, since no loop can exist
// any more. The rwlock stays, since a destroy may hold it here.
static void release_event_loop_slot_table_locked(void) {
  cvector_destroy(ceventloop_slot_table.slots);
  cvector_destroy(ceventloop_slot_table.free_indices);
  ceventloop_slot_table.release_deferred = false;
  if (atomic_exchange(&ceventloop_job_key_bundle.live, false))
    ccol__thread_ls_key_delete(ceventloop_job_key_bundle.key);
  if (atomic_exchange(&queue_dispatch_marker_bundle.live, false))
    ccol__thread_ls_key_delete(queue_dispatch_marker_bundle.key);
}

// Does the release the exit destructor deferred once no slot names a loop.
// The destroy of the last loop calls it; out of line, so the destroy keeps its
// shape. The caller holds the write lock.
static __attribute__((noinline)) void
release_event_loop_slot_table_if_deferred_locked(void) {
  if (ceventloop_slot_table.release_deferred &&
      !event_loop_any_slot_live_locked()) {
    release_event_loop_slot_table_locked();
  }
}

// Frees the loop table at process exit, so valgrind doesn't report it. A live
// loop (another destructor may still own one) defers the release to the
// destroy of the last loop. It runs for every program that links the library,
// so it readies the table through its once first.
__attribute__((destructor)) static void cleanup_event_loop_slot_table(void) {
  ccol__call_once(ceventloop_slot_table.once,
                  ceventloop_slot_table_init_globals);
  if (!ceventloop_slot_table.slots) return;
  ccol__rw_lock_wrlock(ceventloop_slot_table.rwlock);
  if (event_loop_any_slot_live_locked()) {
    ceventloop_slot_table.release_deferred = true;
    ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
    return;
  }
  release_event_loop_slot_table_locked();
  ccol__rw_lock_unlock(ceventloop_slot_table.rwlock);
}
