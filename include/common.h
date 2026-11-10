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

// Common definitions of the library: the version, the pthread wrappers, error
// reporting, the allocator layer, the return codes, type introspection with
// _Generic, and the key/value carrier of the maps. Every other header includes
// it.
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal and can't collide with a symbol of an
// application.
#pragma GCC visibility push(default)

// Version
//
// The release this header belongs to. The root Makefile reads the three numbers
// from here, so the header, the library's file name and the pkg-config version
// always agree. Each number is below 256. CCOL_VERSION_NUMBER packs them as
// 0xMMmmpp, so versions compare as integers (2.1.3 is 0x020103), and
// CCOL_VERSION_STRING spells them as "2.0.0".
#define CCOL_VERSION_MAJOR 2
#define CCOL_VERSION_MINOR 0
#define CCOL_VERSION_PATCH 0
#define CCOL_VERSION_NUMBER \
  ((CCOL_VERSION_MAJOR << 16) | (CCOL_VERSION_MINOR << 8) | CCOL_VERSION_PATCH)
#define CCOL_VERSION_STRING                                    \
  ccol__x_stringify(CCOL_VERSION_MAJOR) "." ccol__x_stringify( \
      CCOL_VERSION_MINOR) "." ccol__x_stringify(CCOL_VERSION_PATCH)

// Gives the version of the library the program runs with, packed like
// CCOL_VERSION_NUMBER. The macros give the version of the header you compiled
// with; use this to learn the one the dynamic loader picked, which can be a
// later release with the same SONAME.
unsigned int ccol_version(void);

// Gives the same version as a static string such as "2.0.0", for logs and
// messages.
const char *ccol_version_string(void);

// Fork safety switch
//
// Some modules (cthreadpool, cthreadcomm, clogger, chttpserver, chttpclient)
// register pthread_atfork() handlers that lock every live handle before a fork,
// so the child never inherits a mutex that stays locked for ever. That costs
// work on every fork() of the process. A program that never forks, or always
// calls exec() right after fork(), can build with -DCCOL_FORK_SAFETY_REQUIRED=0
// to drop those handlers and the state that exists only for them. The modules'
// own locks and the close-on-exec gate stay either way. Define it before the
// first include of this header; the default is 1.
#ifndef CCOL_FORK_SAFETY_REQUIRED
#define CCOL_FORK_SAFETY_REQUIRED 1
#endif

// Internal. OpenBSD's fork() leaves the lock of its handler list held in the
// child, so a pthread_atfork() in a threaded child waits for ever. There every
// module registers its fork handlers at load, never on first use.
#if defined(__OpenBSD__)
#define CCOL__ATFORK_AT_LOAD 1
#endif

// Threading primitives
//
// Every wrapper below is internal. The library reaches pthreads and POSIX
// semaphores only through them, so a port to a system without pthreads replaces
// this header alone. Applications use pthreads or <threads.h> directly.
//
// macOS has no pthread_mutex_timedlock(), pthread_rwlock_timedwrlock(),
// pthread_condattr_setclock() or working sem_init(). CCOL__EMULATE_DARWIN_SYNC
// selects our replacements: a timed lock polls a try-lock, a condition variable
// remembers its clock, and a semaphore is a pipe. It's on by itself on macOS,
// and the Linux and FreeBSD test builds define it to run the same code.
#if defined(__APPLE__) && !defined(CCOL__EMULATE_DARWIN_SYNC)
#define CCOL__EMULATE_DARWIN_SYNC 1
#endif

// The close-on-exec gate. Where a descriptor can't be made close-on-exec in one
// call (sockets and pipes on macOS), the library creates it and sets FD_CLOEXEC
// afterwards, between ccol__cloexec_gate_enter() and
// ccol__cloexec_gate_leave(). Those take the read side of a process-wide lock
// and a fork-prepare handler takes the write side, so a fork() waits until no
// descriptor is between the two steps. posix_spawn(3) runs no fork handler, so
// a macOS program that spawns while the library makes a socket uses
// POSIX_SPAWN_CLOEXEC_DEFAULT. The gate exists only where the two steps exist;
// on Linux and FreeBSD it costs nothing. These functions are hidden.
#if defined(__APPLE__) || defined(CCOL__EMULATE_DARWIN_SYNC) || \
    defined(CCOL__EMULATE_DARWIN_SOCK)
#define CCOL__CLOEXEC_GATE 1
#pragma GCC visibility push(hidden)
void ccol__cloexec_gate_enter(void);
void ccol__cloexec_gate_leave(void);
void ccol__cloexec_gate_register(void);
#pragma GCC visibility pop
#endif

// Mutex wrappers: the type, destroy, init with default attributes, lock,
// unlock, trylock and timedlock. Every module locks through them, so a port
// changes this header only. m is the mutex object (the macros take its
// address). Each evaluates to what the pthread call gives: 0 or an error
// number. trylock gives EBUSY instead of waiting; timedlock gives up with
// ETIMEDOUT at t, an absolute CLOCK_REALTIME struct timespec, through a
// try-lock loop on macOS.
#define ccol__mutex pthread_mutex_t

#define ccol__mutex_destroy(m) pthread_mutex_destroy(&(m))

#define ccol__mutex_init(m) pthread_mutex_init(&(m), NULL)

#define ccol__mutex_lock(m) pthread_mutex_lock(&(m))

#define ccol__mutex_unlock(m) pthread_mutex_unlock(&(m))

#define ccol__mutex_trylock(m) pthread_mutex_trylock(&(m))

#if defined(CCOL__EMULATE_DARWIN_SYNC)
#define ccol__mutex_timedlock(m, t) ccol__mutex_timedlock_poll(&(m), &(t))
#else
#define ccol__mutex_timedlock(m, t) pthread_mutex_timedlock(&(m), &(t))
#endif

// Read-write lock wrappers: the type, destroy, init, write lock, read lock,
// timed write lock and unlock, for the slot tables and registries that many
// threads read and few write. a is the lock object. Each evaluates to 0 or an
// error number; the timed write lock takes the same absolute CLOCK_REALTIME
// deadline t as ccol__mutex_timedlock.
#define ccol__rw_lock pthread_rwlock_t

#define ccol__rw_lock_destroy(a) pthread_rwlock_destroy(&(a))

#define ccol__rw_lock_init(a) pthread_rwlock_init(&(a), NULL)

// Initializes again, in the child of fork(), the rwlock a that the forking
// thread held for writing (as a fork-prepare handler leaves it). The fork child
// handlers need it because a plain unlock doesn't always release such a lock
// there: glibc checks the writer's thread ID, and the child's thread has a new
// one. macOS refuses to init an object it still sees as a lock in use, so the
// impl clears it first. Evaluates to 0 or an error number. The impl takes
// void * because a strict -std=c11 <pthread.h> declares no rwlock type.
#define ccol__rw_lock_reinit_in_child(a) \
  ccol__rw_lock_reinit_in_child_impl(&(a))
#pragma GCC visibility push(hidden)
int ccol__rw_lock_reinit_in_child_impl(void *lock);
#pragma GCC visibility pop

#define ccol__rw_lock_wrlock(a) pthread_rwlock_wrlock(&(a))

#define ccol__rw_lock_rdlock(a) pthread_rwlock_rdlock(&(a))

#if defined(CCOL__EMULATE_DARWIN_SYNC)
#define ccol__rw_lock_timedwrlock(a, t) \
  ccol__rw_lock_timedwrlock_poll(&(a), &(t))
#else
#define ccol__rw_lock_timedwrlock(a, t) pthread_rwlock_timedwrlock(&(a), &(t))
#endif

#define ccol__rw_lock_unlock(a) pthread_rwlock_unlock(&(a))

// Condition variable wrappers: the types, attribute init, destroy and setclock,
// init with default or given attributes (ca), wait, timed wait, signal and
// broadcast. Queues, pools and the logger wait on them, and the monotonic clock
// needs setclock. c is the condition variable and m its mutex, unlocked while
// waiting. Each evaluates to 0 or an error number; the timed wait gives
// ETIMEDOUT at t, an absolute deadline on the clock of c. On macOS the second
// set below emulates the clock.
#if !defined(CCOL__EMULATE_DARWIN_SYNC)
#define ccol__cond_var pthread_cond_t

#define ccol__cond_var_attr pthread_condattr_t

#define ccol__cond_var_attr_destroy(ca) pthread_condattr_destroy(&(ca))

#define ccol__cond_var_attr_init(ca) pthread_condattr_init(&(ca))

#define ccol__cond_var_attr_setclock(ca, clk) \
  pthread_condattr_setclock(&(ca), clk)

#define ccol__cond_var_destroy(c) pthread_cond_destroy(&(c))

#define ccol__cond_var_init(c) pthread_cond_init(&(c), NULL)

#define ccol__cond_var_init_ca(c, ca) pthread_cond_init(&(c), &(ca))

#define ccol__cond_var_wait(c, m) pthread_cond_wait(&(c), &(m))

#define ccol__cond_var_timedwait(c, m, t) \
  pthread_cond_timedwait(&(c), &(m), &(t))

#define ccol__cond_var_signal(c) pthread_cond_signal(&(c))

#define ccol__cond_var_broadcast(c) pthread_cond_broadcast(&(c))
#else
// The emulated condition variable and its attributes remember the clock of
// their deadlines (see CCOL__EMULATE_DARWIN_SYNC).
typedef struct {
  pthread_cond_t cond;
  clockid_t clock;
} ccol__cond_var_s;
typedef struct {
  clockid_t clock;
} ccol__cond_var_attr_s;

// Sets the clock of emulated condition variable attributes and gives 0. The
// attribute macros of the emulation use it, since there is no
// pthread_condattr_setclock() on macOS.
static inline int ccol__cond_var_attr_set(ccol__cond_var_attr_s *ca,
                                          clockid_t clk) {
  ca->clock = clk;
  return 0;
}
// Initializes an emulated condition variable that remembers clk, so
// ccol__cond_var_timedwait_clock knows how to read its deadlines. Gives what
// pthread_cond_init() gives.
static inline int ccol__cond_var_init_clock(ccol__cond_var_s *c,
                                            clockid_t clk) {
  c->clock = clk;
  return pthread_cond_init(&c->cond, NULL);
}

#define ccol__cond_var ccol__cond_var_s
#define ccol__cond_var_attr ccol__cond_var_attr_s
#define ccol__cond_var_attr_destroy(ca) \
  ccol__cond_var_attr_set(&(ca), (ca).clock)
#define ccol__cond_var_attr_init(ca) \
  ccol__cond_var_attr_set(&(ca), CLOCK_REALTIME)
#define ccol__cond_var_attr_setclock(ca, clk) \
  ccol__cond_var_attr_set(&(ca), (clk))
#define ccol__cond_var_destroy(c) pthread_cond_destroy(&(c).cond)
#define ccol__cond_var_init(c) ccol__cond_var_init_clock(&(c), CLOCK_REALTIME)
#define ccol__cond_var_init_ca(c, ca) \
  ccol__cond_var_init_clock(&(c), (ca).clock)
#define ccol__cond_var_wait(c, m) pthread_cond_wait(&(c).cond, &(m))
#define ccol__cond_var_timedwait(c, m, t) \
  ccol__cond_var_timedwait_clock(&(c), &(m), &(t))
#define ccol__cond_var_signal(c) pthread_cond_signal(&(c).cond)
#define ccol__cond_var_broadcast(c) pthread_cond_broadcast(&(c).cond)
#endif

// Semaphore wrappers: the type, init with count value, destroy, wait and post,
// on an unnamed process-private semaphore s. They exist for the one
// synchronization a signal handler may do: ccol__semaphore_post is
// async-signal-safe (POSIX says so for sem_post(3)), while mutexes, condition
// variables, thread creation and call_once are not. An entry point that must
// work from a signal handler posts to a watcher thread that does the real work,
// as the engine-stop watcher of chttpserver does. Init, destroy, wait and post
// evaluate to 0, or -1 with errno set. wait may block, so never call it from a
// handler.
#if !defined(CCOL__EMULATE_DARWIN_SYNC)
#define ccol__semaphore sem_t

#define ccol__semaphore_init(s, value) sem_init(&(s), 0, (value))

#define ccol__semaphore_destroy(s) sem_destroy(&(s))

#define ccol__semaphore_wait(s) sem_wait(&(s))

#define ccol__semaphore_post(s) sem_post(&(s))
#else
// The emulated semaphore is a pipe, one byte per unit. The write end doesn't
// block, so a post on a full pipe leaves the count as it is and still succeeds.
// That's safe because every user sets a request flag before it posts and the
// waiter reads every flag after each wake.
typedef struct {
  int fd[2];
} ccol__semaphore_s;
#define ccol__semaphore ccol__semaphore_s
#define ccol__semaphore_init(s, value) ccol__semaphore_pipe_init(&(s), (value))
#define ccol__semaphore_destroy(s) ccol__semaphore_pipe_destroy(&(s))
#define ccol__semaphore_wait(s) ccol__semaphore_pipe_wait(&(s))
#define ccol__semaphore_post(s) ccol__semaphore_pipe_post(&(s))
#endif

#if defined(CCOL__EMULATE_DARWIN_SYNC)
#include <fcntl.h>

// True once the clock clk has reached the absolute time t. The try-lock loops
// use it to stop at their deadline.
static inline bool ccol__time_reached(clockid_t clk, const struct timespec *t) {
  struct timespec now;
  clock_gettime(clk, &now);
  return now.tv_sec > t->tv_sec ||
         (now.tv_sec == t->tv_sec && now.tv_nsec >= t->tv_nsec);
}

// Sleeps one pause of a try-lock loop and updates *us, the length of the next:
// 20 microseconds at first, doubling up to 1 ms, so a short wait is cheap and a
// long one doesn't burn CPU.
static inline void ccol__try_lock_pause(unsigned *us) {
  *us = *us ? (*us >= 500u ? 1000u : *us * 2u) : 20u;
  struct timespec ts = {0, (long)*us * 1000L};
  nanosleep(&ts, NULL);
}

// Locks m or gives up at t on CLOCK_REALTIME, as a try-lock loop, because macOS
// has no pthread_mutex_timedlock(). Gives 0, ETIMEDOUT, or the try-lock's
// error.
static inline int ccol__mutex_timedlock_poll(pthread_mutex_t *m,
                                             const struct timespec *t) {
  unsigned us = 0;
  for (;;) {
    int rc = pthread_mutex_trylock(m);
    if (rc != EBUSY) return rc;
    if (ccol__time_reached(CLOCK_REALTIME, t)) return ETIMEDOUT;
    ccol__try_lock_pause(&us);
  }
}

// Takes the write side of a or gives up at t on CLOCK_REALTIME, as a try-lock
// loop, because macOS has no pthread_rwlock_timedwrlock(). Gives 0, ETIMEDOUT,
// or the try-lock's error.
static inline int ccol__rw_lock_timedwrlock_poll(pthread_rwlock_t *a,
                                                 const struct timespec *t) {
  unsigned us = 0;
  for (;;) {
    int rc = pthread_rwlock_trywrlock(a);
    if (rc != EBUSY) return rc;
    if (ccol__time_reached(CLOCK_REALTIME, t)) return ETIMEDOUT;
    ccol__try_lock_pause(&us);
  }
}

// Waits on c with m held until a deadline t on the clock that c remembers.
// It's the emulated ccol__cond_var_timedwait, needed because macOS can't set a
// condition variable's clock. A realtime deadline goes straight to
// pthread_cond_timedwait(). A monotonic one becomes the time left, worked out
// field by field so a saturated deadline can't overflow, and cut to 10^9
// seconds (every caller waits in a loop that checks its condition). macOS waits
// for it with pthread_cond_timedwait_relative_np(); a test build elsewhere adds
// it to the realtime clock, saturating at the largest time_t so a 32-bit time_t
// never wraps into the past. Gives 0 or ETIMEDOUT.
static inline int ccol__cond_var_timedwait_clock(ccol__cond_var_s *c,
                                                 pthread_mutex_t *m,
                                                 const struct timespec *t) {
  if (c->clock != CLOCK_MONOTONIC)
    return pthread_cond_timedwait(&c->cond, m, t);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec rel = {0, 0};
  if (t->tv_sec > now.tv_sec ||
      (t->tv_sec == now.tv_sec && t->tv_nsec > now.tv_nsec)) {
    if ((long long)t->tv_sec - (long long)now.tv_sec > 1000000000LL) {
      rel.tv_sec = (time_t)1000000000;
    } else {
      rel.tv_sec = t->tv_sec - now.tv_sec;
      rel.tv_nsec = t->tv_nsec - now.tv_nsec;
      if (rel.tv_nsec < 0) {
        rel.tv_sec--;
        rel.tv_nsec += 1000000000L;
      }
    }
  }
#if defined(__APPLE__)
  return pthread_cond_timedwait_relative_np(&c->cond, m, &rel);
#else
  struct timespec at;
  clock_gettime(CLOCK_REALTIME, &at);
  const long long tmax =
      sizeof(time_t) >= 8 ? (long long)INT64_MAX : (long long)INT32_MAX;
  if ((long long)rel.tv_sec > tmax - 1 - (long long)at.tv_sec) {
    at.tv_sec = (time_t)tmax;
    at.tv_nsec = 999999999L;
  } else {
    at.tv_sec += rel.tv_sec;
    at.tv_nsec += rel.tv_nsec;
    if (at.tv_nsec >= 1000000000L) {
      at.tv_sec++;
      at.tv_nsec -= 1000000000L;
    }
  }
  return pthread_cond_timedwait(&c->cond, m, &at);
#endif
}

// Initializes s as a semaphore with count value: a pipe holding value bytes,
// with a write end that doesn't block. It replaces sem_init(), which doesn't
// work on macOS. Both ends become close-on-exec inside the gate. Gives 0, or -1
// with errno set; a value the pipe can't hold fails with EINVAL.
static inline int ccol__semaphore_pipe_init(ccol__semaphore_s *s,
                                            unsigned value) {
  ccol__cloexec_gate_enter();
  if (pipe(s->fd) != 0) {
    int saved = errno;
    ccol__cloexec_gate_leave();
    errno = saved;
    return -1;
  }
  (void)fcntl(s->fd[0], F_SETFD, FD_CLOEXEC);
  (void)fcntl(s->fd[1], F_SETFD, FD_CLOEXEC);
  ccol__cloexec_gate_leave();
  int wfl = fcntl(s->fd[1], F_GETFL);
  if (wfl < 0 || fcntl(s->fd[1], F_SETFL, wfl | O_NONBLOCK) != 0) {
    int saved = errno;
    close(s->fd[0]);
    close(s->fd[1]);
    errno = saved;
    return -1;
  }
  for (unsigned i = 0; i < value; i++) {
    ssize_t n;
    do {
      n = write(s->fd[1], "", 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) {
      int saved =
          (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ? errno : EINVAL;
      close(s->fd[0]);
      close(s->fd[1]);
      errno = saved;
      return -1;
    }
  }
  return 0;
}

// Destroys the pipe semaphore s by closing both ends, like sem_destroy(). Gives
// 0.
static inline int ccol__semaphore_pipe_destroy(ccol__semaphore_s *s) {
  close(s->fd[0]);
  close(s->fd[1]);
  return 0;
}

// Waits until the count of s is above 0 and takes one unit, by reading one
// byte, like sem_wait(). Gives 0, or -1 with errno set; the end of the pipe and
// a descriptor that isn't open give EINVAL, as sem_wait() does for an invalid
// semaphore.
static inline int ccol__semaphore_pipe_wait(ccol__semaphore_s *s) {
  char b;
  for (;;) {
    ssize_t n = read(s->fd[0], &b, 1);
    if (n == 1) return 0;
    if (n < 0 && errno == EINTR) continue;
    if (n >= 0 || errno == EBADF) errno = EINVAL;
    return -1;
  }
}

// Adds one unit to s by writing one byte, like sem_post(), and is
// async-signal-safe like write(2), which is why the semaphore exists. A full
// pipe counts as success. Gives 0 or -1. errno keeps its value on success, so a
// post from a signal handler doesn't change the errno of the thread it
// interrupts.
static inline int ccol__semaphore_pipe_post(ccol__semaphore_s *s) {
  int saved = errno;
  for (;;) {
    ssize_t n = write(s->fd[1], "", 1);
    if (n == 1) {
      errno = saved;
      return 0;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      errno = saved;
      return 0;
    }
    return -1;
  }
}
#endif

// Thread wrappers: the id type, the caller's id, id comparison, create, join,
// signal mask and kill, for the worker, poller and writer threads of the
// modules. ccol__thread_create runs fn(arg) with default attributes and stores
// the thread in handle; join waits for it. sigmask takes how, set and oldset as
// pthread_sigmask() does, and kill sends sig to handle; both need <signal.h> in
// the caller. All evaluate to 0 or an error number, except equal (nonzero for
// the same thread) and get_thread_id.
#define ccol__thread_id pthread_t

#define ccol__get_thread_id pthread_self

#define ccol__thread_id_equal(a, b) pthread_equal((a), (b))

#define ccol__thread_create(handle, fn, arg) \
  pthread_create(&(handle), NULL, (fn), (arg))

#define ccol__thread_join(handle) pthread_join((handle), NULL)

#define ccol__thread_sigmask(how, set, oldset) \
  pthread_sigmask((how), (set), (oldset))

#define ccol__thread_kill(handle, sig) pthread_kill((handle), (sig))

// One-time initialization: the flag type, its static initializer and
// ccol__call_once, which runs fn exactly once for every caller that races on
// flag and evaluates to 0 or an error number. The library initializes every
// lock lazily behind a once flag, so a port never needs a static lock
// initializer; CCOL__ONCE_INIT is the only static initializer this file offers,
// because a once flag holds a plain "not run yet" state.
#define ccol__once_flag pthread_once_t

#define CCOL__ONCE_INIT PTHREAD_ONCE_INIT

#define ccol__call_once(flag, fn) pthread_once(&(flag), (fn))

// Thread-local storage key wrappers: the type, create with a destructor that
// runs at thread exit, delete, and set and get of the calling thread's value.
// The per-thread caches of cjson, cyaml, cmempool and clogger hang off them.
// create, delete and set evaluate to 0 or an error number; get gives the value
// or NULL.
#define ccol__thread_ls_key pthread_key_t

#define ccol__thread_ls_key_create(key, destructor) \
  pthread_key_create(&(key), (destructor))

#define ccol__thread_ls_key_delete(key) pthread_key_delete((key))

#define ccol__thread_ls_set(key, val) pthread_setspecific((key), (val))

#define ccol__thread_ls_get(key) pthread_getspecific((key))

// Register prepare, parent and child handlers around fork(2), which the fork
// safety of the modules needs. They evaluate to what pthread_atfork() gives, 0
// or an error number. Where the close-on-exec gate exists, ccol__at_fork
// registers the gate's handlers first, once, so the gate's prepare handler runs
// after every other prepare handler of the library. Library code uses
// ccol__at_fork, never the raw form.
#define ccol__at_fork_raw(prepare, parent, child) \
  pthread_atfork((prepare), (parent), (child))
#if defined(CCOL__CLOEXEC_GATE)
#define ccol__at_fork_first() ccol__cloexec_gate_register()
#endif
#if defined(ccol__at_fork_first)
#define ccol__at_fork(prepare, parent, child) \
  (ccol__at_fork_first(), ccol__at_fork_raw(prepare, parent, child))
#else
#define ccol__at_fork(prepare, parent, child) \
  ccol__at_fork_raw(prepare, parent, child)
#endif

// Copies the name of thread into buf (len bytes), for clogger's thread identity
// on systems that can't read the name from /proc. Evaluates to 0 or an error
// number; OpenBSD's pthread_get_name_np() gives no result, so there it's
// always 0.
#if defined(__OpenBSD__)
#define ccol__get_thread_name_np(thread, buf, len) \
  (pthread_get_name_np((thread), (buf), (len)), 0)
#else
#define ccol__get_thread_name_np(thread, buf, len) \
  pthread_getname_np((thread), (buf), (len))
#endif

// Renames the calling thread to name, for the non-Linux branches of
// ccol_set_thread_name(). Evaluates to 0 or an error number. macOS takes only
// the name, FreeBSD the thread too, and OpenBSD's pthread_set_name_np() gives
// no result, so there it's always 0.
#if defined(__APPLE__)
#define ccol__set_own_thread_name_np(name) pthread_setname_np((name))
#elif defined(__OpenBSD__)
#define ccol__set_own_thread_name_np(name) \
  (pthread_set_name_np(pthread_self(), (name)), 0)
#else
#define ccol__set_own_thread_name_np(name) \
  pthread_setname_np(pthread_self(), (name))
#endif

// macOS only: stores the 64-bit numeric id of thread in *out_id, for the thread
// id that clogger writes into each record. Evaluates to 0 or an error number.
#define ccol__get_thread_id_np(thread, out_id) \
  pthread_threadid_np((thread), (out_id))

// FreeBSD only: evaluates to the kernel thread id of the calling thread, for
// the thread id that clogger writes into each record. The caller includes
// <pthread_np.h>.
#if defined(__FreeBSD__)
#define ccol__get_own_thread_os_id_np() pthread_getthreadid_np()
#endif

// Checks cond, a condition that must hold, and calls abort() after printing the
// file, line and condition when it's false. It's the library's assert: many
// checks (double free, a foreign pointer, corruption) are part of the contract,
// so unlike assert() this always evaluates cond and never compiles away under
// NDEBUG. It's a statement with no value.
#define ccol__assert(cond)                                                   \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "%s:%d: ccol_assert failed: %s\n", __FILE__, __LINE__, \
              #cond);                                                        \
      abort();                                                               \
    }                                                                        \
  } while (0)

// Error handling
//
// Turn s into a string literal, as written (ccol__stringify) or after macro
// expansion (ccol__x_stringify). CCOL__ERR_STR and the version string need the
// expanded form for __LINE__ and the version numbers.
#define ccol__stringify(s) #s

#define ccol__x_stringify(s) ccol__stringify(s)

// Evaluates to the identifier prefix_n, unique in the translation unit when n
// is
// __COUNTER__. The container macros name every temporary this way: a temporary
// is in scope inside its own initializer, where the caller's argument is
// evaluated, so nested public macros (chmap_get(m, chmap_get(m, k))) would
// otherwise shadow each other under -Wshadow. The second level makes n expand
// before the paste.
#define ccol__uniq(prefix, n) ccol__uniq_paste(prefix, n)
#define ccol__uniq_paste(prefix, n) prefix##_##n

// Expands to one _Generic association that maps the type of C23 nullptr to v,
// with its comma, or to nothing when the compiler has no nullptr. A classifying
// _Generic adds it so that nullptr is classified like a void * NULL instead of
// falling to the default. GCC 13 and Clang 16 report a __STDC_VERSION__ below
// 202311L for -std=c2x, so the test names them.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L && \
    ((defined(__clang__) && __clang_major__ >= 16) ||          \
     (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 13))
#define CCOL__NULLPTR_ASSOC(v) __typeof__(nullptr) : v,
#else
#define CCOL__NULLPTR_ASSOC(v)
#endif

// Defined for a Clang that knows -Wunreachable-code-generic-assoc. The
// classifying macros list qualified associations on purpose, so they have a
// form that turns the warning off for such a compiler. An older Clang takes the
// plain form, since naming an unknown warning is a diagnostic of its own.
#if defined(__clang__) && defined(__has_warning)
#if __has_warning("-Wunreachable-code-generic-assoc")
#define CCOL__CLANG_GENERIC_ASSOC_WARNING 1
#endif
#endif

// Every err and err_str out-parameter of the library points to storage the
// library owns; never free it. Most are CCOL__ERR_STR() literals that live for
// the whole process. cjson and cyaml build their parse messages in a per-thread
// buffer, valid until the next failing call of the same kind on the same thread
// (like strerror(3)). A string the caller does own is never called err.
//
// CCOL__ERR_STR builds such a literal from the message x: "file:line - x". The
// file and line tell a caller where in the library the error came from.
#define CCOL__ERR_STR(x) (__FILE__ ":" ccol__x_stringify(__LINE__) " - " x)

// Prints a printf-style message (format first, then its arguments) with the
// file and line to stderr in one fprintf(), and stops the program with
// ccol__assert(false). The library and the type-inferred macros use it for
// errors nobody can recover from, such as a failed chmap_get. It's a statement
// with no value. The public macro appends an empty string that the trailing %s
// of the format prints, so the argument list after the format is never empty
// and no GNU comma extension is needed; the installed headers must build under
// -std=c11 -pedantic.
#define ccol__fatal_err(...) ccol__fatal_err_impl(__VA_ARGS__, "")

#define ccol__fatal_err_impl(ccol__err_fmt, ...)                               \
  do {                                                                         \
    fprintf(stderr, "%s:%d: fatal: " ccol__err_fmt "%s\n", __FILE__, __LINE__, \
            __VA_ARGS__);                                                      \
    ccol__assert(false);                                                       \
  } while (0)

// Memory management
//
// The default allocator: malloc, calloc, realloc and free with the same
// arguments and results. ccol__mem_* and the containers use them when they got
// no procs.
#define ccol__default_mem_alloc(size) malloc((size))

#define ccol__default_mem_calloc(elem_count, elem_size) \
  calloc((elem_count), (elem_size))

#define ccol__default_mem_realloc(ptr, new_size) realloc((ptr), (new_size))

#define ccol__default_mem_free(ptr) free((ptr))

// Allocate, zero-allocate, reallocate and free through m_procs, or through the
// default allocator when it's NULL. Every allocation of the library goes
// through them, so a container honours the procs it was created with. The
// arguments after m_procs are those of the standard function, and each
// evaluates to what that function gives. The outer parentheses matter: ?: binds
// more loosely than a cast or +, so without them (char *)ccol__mem_alloc(mp, n)
// + off would cast m_procs and add the offset to one arm only, and it would
// compile silently.
#define ccol__mem_alloc(m_procs, size) \
  ((m_procs) ? (m_procs)->malloc((size)) : ccol__default_mem_alloc((size)))

#define ccol__mem_calloc(m_procs, e_count, e_size)    \
  ((m_procs) ? (m_procs)->calloc((e_count), (e_size)) \
             : ccol__default_mem_calloc((e_count), (e_size)))

#define ccol__mem_realloc(m_procs, ptr, new_size)    \
  ((m_procs) ? (m_procs)->realloc((ptr), (new_size)) \
             : ccol__default_mem_realloc((ptr), (new_size)))

#define ccol__mem_free(m_procs, ptr) \
  ((m_procs) ? (m_procs)->free((ptr)) : ccol__default_mem_free((ptr)))

// The invalid size_t (its largest value). Functions that give a size or an
// index give it for "none" or a failure.
#define ccol_invalid_size ((size_t)-1)

// Internal: the largest power of two a size_t can hold (2^31 or 2^63), the cap
// of the power-of-two growth of the containers.
#define ccol__max_power_of_two_size \
  ((size_t)1 << ((sizeof(size_t) * CHAR_BIT) - 1))

// The largest element count of a collection; a growth past it fails.
#define ccol_max_elem_count ccol__max_power_of_two_size

// Return codes
//
// The result of a collection operation: zero is success and every error is
// negative. Each enumerator has an explicit value on purpose. Some callers test
// "== 0", and with implicit values a new enumerator in the middle would
// renumber every later one, ccol_success included. Add new codes with explicit
// values.
typedef enum ccollections_retval {
  ccol_unexpected_failure = -18,
  // HTTP client failures. tls_cert_load_failed is a local certificate, key or
  // CA file that can't be read or parsed; transfer_aborted is a send or receive
  // error or a stream callback that aborted.
  ccol_http_connection_failed = -17,
  ccol_http_host_resolution_failed = -16,
  ccol_http_tls_handshake_failed = -15,
  ccol_http_tls_cert_verification_failed = -14,
  ccol_http_tls_cert_load_failed = -13,
  ccol_http_too_many_redirects = -12,
  ccol_http_invalid_url = -11,
  ccol_http_transfer_aborted = -10,
  // msg_too_large: a message above the configured size limit.
  ccol_msg_too_large = -9,
  ccol_container_empty = -8,
  ccol_container_full = -7,
  ccol_timed_out = -6,
  ccol_not_permitted = -5,
  ccol_invalid_args = -4,
  ccol_key_not_found = -3,
  ccol_key_already_present = -2,
  ccol_not_enough_memory = -1,
  ccol_success = 0
} ccol_retval;

// Gives the name of r as a string literal ("unknown" for a value it doesn't
// know), handy in a log or a ccol__fatal_err() message.
static inline const char *ccol_retval_to_str(ccol_retval r) {
  switch (r) {
    case ccol_success:
      return "ccol_success";
    case ccol_not_enough_memory:
      return "ccol_not_enough_memory";
    case ccol_key_already_present:
      return "ccol_key_already_present";
    case ccol_key_not_found:
      return "ccol_key_not_found";
    case ccol_invalid_args:
      return "ccol_invalid_args";
    case ccol_not_permitted:
      return "ccol_not_permitted";
    case ccol_timed_out:
      return "ccol_timed_out";
    case ccol_container_full:
      return "ccol_container_full";
    case ccol_container_empty:
      return "ccol_container_empty";
    case ccol_msg_too_large:
      return "ccol_msg_too_large";
    case ccol_http_connection_failed:
      return "ccol_http_connection_failed";
    case ccol_http_host_resolution_failed:
      return "ccol_http_host_resolution_failed";
    case ccol_http_tls_handshake_failed:
      return "ccol_http_tls_handshake_failed";
    case ccol_http_tls_cert_verification_failed:
      return "ccol_http_tls_cert_verification_failed";
    case ccol_http_tls_cert_load_failed:
      return "ccol_http_tls_cert_load_failed";
    case ccol_http_too_many_redirects:
      return "ccol_http_too_many_redirects";
    case ccol_http_invalid_url:
      return "ccol_http_invalid_url";
    case ccol_http_transfer_aborted:
      return "ccol_http_transfer_aborted";
    case ccol_unexpected_failure:
      return "ccol_unexpected_failure";
    default:
      return "unknown";
  }
}

// Writes a hex dump of the size bytes at data to stderr (offset, hex columns,
// printable ASCII). The cbmap and chmap macros call it before a
// ccol__fatal_err() for a failed insert or lookup, so you can see a key that is
// binary data.
static inline void ccol__dump_key_to_stderr(const void *data, size_t size) {
  const unsigned char *p = (const unsigned char *)data;
  fprintf(stderr, "Key dump (%zu byte%s):\n", size, size == 1 ? "" : "s");
  for (size_t i = 0; i < size; i += 16) {
    fprintf(stderr, "  %08zx  ", i);
    for (size_t j = 0; j < 16; j++) {
      if (i + j < size)
        fprintf(stderr, "%02x ", p[i + j]);
      else
        fprintf(stderr, "   ");
      if (j == 7) fprintf(stderr, " ");
    }
    fprintf(stderr, " |");
    for (size_t j = 0; j < 16 && i + j < size; j++)
      fprintf(stderr, "%c", isprint(p[i + j]) ? (char)p[i + j] : '.');
    fprintf(stderr, "|\n");
  }
}

// Attaches the cleanup function destructor to a variable declaration; it runs
// with a pointer to the variable when the variable goes out of scope. The
// _scoped forms of the containers and ccol_scoped_ptr use it to destroy a
// handle on every way out of a scope.
#define ccol__destructor(destructor) __attribute__((cleanup(destructor)))

// Custom memory management types
//
// The signatures of a custom malloc, free, calloc and realloc.
typedef void *(*ccol_malloc)(size_t size);

typedef void (*ccol_free)(void *ptr);

typedef void *(*ccol_calloc)(size_t elem_count, size_t elem_size);

typedef void *(*ccol_realloc)(void *ptr, size_t size);

// A custom allocator. Give it to a collection's _mp or _full constructor. All
// four functions must be non-NULL and behave like the standard ones, including
// alignment: every block must be aligned for any object type
// (_Alignof(max_align_t)), since containers put such entries at the start of a
// block.
typedef struct ccol_memmgmt_procs {
  ccol_malloc malloc;
  ccol_free free;
  ccol_calloc calloc;
  ccol_realloc realloc;
} ccol_memmgmt_procs;

// Comparison and hashing types
//
// A comparison for sorts and ordered maps: negative when first < second, zero
// when equal, positive when first > second.
typedef int (*ccol_comparison_proc)(const void *first, const void *second);

// A hash for hash maps; a good one spreads the values evenly. size is the map's
// key size for a fixed-size key type, or the byte length the caller gives for a
// string or other variable-length key, so the function always hashes exactly
// ptr[0..size).
typedef unsigned long (*ccol_hashing_proc)(const void *ptr, size_t size);

// A key equality for hash maps, used with a ccol_hashing_proc. It gives true
// when the two keys are the same key, and two keys it calls equal must hash the
// same, or a lookup misses a key that is present. The pointers carry no
// alignment for the key type, so read fields through memcpy.
typedef bool (*ccol_key_equality_proc)(const void *first, size_t first_size,
                                       const void *second, size_t second_size);

// Map key-value types
//
// A pointer and a size that carry a key or a value of any type to a map, which
// copies the bytes. For a string, size counts the NUL.
typedef struct cmap_pair {
  void *ptr;
  size_t size;
} cmap_pair;

// The iterator of every container. The pointers are valid until the container
// changes, and you must destroy the iterator unless the walk reached its end.
typedef struct cmap_iterator {
  // Both point at the container's own accessor of the current entry. You can
  // write the bytes they address, but not the pairs: a stored pointer the
  // container never allocated would be freed by cbstmap's teardown.
  const cmap_pair *key_pair;
  const cmap_pair *val_pair;
  // Internal: advance and destroy functions of the container.
  struct cmap_iterator *(*ccol__next_fn)(struct cmap_iterator *);
  void (*ccol__free_fn)(struct cmap_iterator *);
  // true when val_pair->ptr is the element itself (cvector); false when the
  // map's inline-storage rules apply.
  bool ccol__direct_ptr;
} cmap_iterator;

// Memory management utilities
//
// Checks a caller's allocator before a container stores it, so a missing
// function fails the create instead of crashing later. Evaluates to true when
// mmgt_procs is NULL or all four functions are set, and to false otherwise,
// with *err set to a library-owned string when err isn't NULL.
#define ccol__verify_memmgmt_procs(mmgt_procs, err)                        \
  ({                                                                       \
    bool ccol__vmp_ok = true;                                              \
    if ((mmgt_procs) && (!(mmgt_procs)->malloc || !(mmgt_procs)->calloc || \
                         !(mmgt_procs)->realloc || !(mmgt_procs)->free)) { \
      if ((err)) {                                                         \
        *(err) = CCOL__ERR_STR(                                            \
            "Detected at least one NULL memory management function");      \
      }                                                                    \
      ccol__vmp_ok = false;                                                \
    }                                                                      \
    ccol__vmp_ok;                                                          \
  })

// Copies mmgmt_procs into a block allocated with it and stores that in
// container->m_procs, so the container doesn't depend on the caller's struct
// staying alive. A NULL mmgmt_procs stores NULL (the default allocator).
// Evaluates to true, or to false with *err set when the allocation fails.
#define ccol__populate_mem_mgmt_procs(container, mmgmt_procs, err) \
  ({                                                               \
    bool ccol__pmp_ok = true;                                      \
    if ((mmgmt_procs)) {                                           \
      (container)->m_procs =                                       \
          (mmgmt_procs)->malloc(sizeof(ccol_memmgmt_procs));       \
      if (!(container)->m_procs) {                                 \
        if ((err)) {                                               \
          *(err) = CCOL__ERR_STR(                                  \
              "Failed to allocate buffer for memory mgmt buffer"); \
        }                                                          \
        ccol__pmp_ok = false;                                      \
      } else {                                                     \
        memcpy((container)->m_procs, (mmgmt_procs),                \
               sizeof(ccol_memmgmt_procs));                        \
      }                                                            \
    } else {                                                       \
      (container)->m_procs = NULL;                                 \
    }                                                              \
    ccol__pmp_ok;                                                  \
  })

// Internal: what the cleanup of ccol_scoped_ptr needs at the end of the scope,
// the address of the guarded pointer and the allocator that frees it. The
// macros fill it in.
typedef struct ccol__scoped_ptr_ctx {
  void **ptr_addr;
  ccol_memmgmt_procs *m_procs;
} ccol__scoped_ptr_ctx;

// Frees the current value of the pointer that ctx guards, if any, with ctx's
// allocator, and sets it to NULL. ccol_scoped_ptr_mp attaches it as the cleanup
// that runs at the end of the scope.
static inline void ccol__scoped_ptr_cleanup(ccol__scoped_ptr_ctx *ctx) {
  if (ctx->ptr_addr && *ctx->ptr_addr) {
    ccol__mem_free(ctx->m_procs, *ctx->ptr_addr);
    *ctx->ptr_addr = NULL;
  }
}

// Declares type *name = NULL and frees whatever name holds at the end of the
// enclosing scope, with mmgmt_procs->free or with free() when mmgmt_procs is
// NULL. It saves the free on every return path of a function that owns a
// buffer. mmgmt_procs is kept by reference, so it must outlive the scope. Only
// the last value is freed, so free an earlier value yourself before you assign
// a new one. It's two declarations, so use it as a statement of its own. The
// hidden context is marked unused because Clang 11 doesn't count a cleanup as a
// use.
#define ccol_scoped_ptr_mp(name, type, mmgmt_procs)                    \
  type *name = NULL;                                                   \
  ccol__scoped_ptr_ctx name##__ccol_scoped_ctx __attribute__((unused)) \
  ccol__destructor(ccol__scoped_ptr_cleanup) = {(void **)&(name),      \
                                                (mmgmt_procs)}

// ccol_scoped_ptr_mp() with the default allocator, for the common case of a
// malloc'ed buffer.
#define ccol_scoped_ptr(name, type) ccol_scoped_ptr_mp(name, type, NULL)

// Evaluates to the current value of name, a ccol_scoped_ptr variable, and sets
// name to NULL, so the scoped cleanup does nothing. Use it to hand the pointer
// to code outside the scope, as a return value for example.
#define ccol_scoped_ptr_release(name)             \
  ({                                              \
    __typeof__(name) ccol__released_ptr = (name); \
    (name) = NULL;                                \
    ccol__released_ptr;                           \
  })

// Type introspection
//
// Evaluates to true when x has a standard integer or floating-point type, const
// or not, and to false otherwise. chmap uses it to choose open addressing for
// numeric keys and values. Only the type of x is read. The Clang form turns
// -Wunreachable-code-generic-assoc off around the _Generic.
#if defined(CCOL__CLANG_GENERIC_ASSOC_WARNING)
#define ccol__is_integral_type(x)                                           \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool ccol__iit_result = _Generic((x),                                   \
        char: true,                                                         \
        signed char: true,                                                  \
        short: true,                                                        \
        int: true,                                                          \
        long: true,                                                         \
        long long: true,                                                    \
        unsigned char: true,                                                \
        unsigned short: true,                                               \
        unsigned int: true,                                                 \
        unsigned long: true,                                                \
        unsigned long long: true,                                           \
        float: true,                                                        \
        double: true,                                                       \
        long double: true,                                                  \
        const char: true,                                                   \
        const signed char: true,                                            \
        const short: true,                                                  \
        const int: true,                                                    \
        const long: true,                                                   \
        const long long: true,                                              \
        const unsigned char: true,                                          \
        const unsigned short: true,                                         \
        const unsigned int: true,                                           \
        const unsigned long: true,                                          \
        const unsigned long long: true,                                     \
        const float: true,                                                  \
        const double: true,                                                 \
        const long double: true,                                            \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    ccol__iit_result;                                                       \
  })
#else
#define ccol__is_integral_type(x)     \
  _Generic((x),                       \
      char: true,                     \
      signed char: true,              \
      short: true,                    \
      int: true,                      \
      long: true,                     \
      long long: true,                \
      unsigned char: true,            \
      unsigned short: true,           \
      unsigned int: true,             \
      unsigned long: true,            \
      unsigned long long: true,       \
      float: true,                    \
      double: true,                   \
      long double: true,              \
      const char: true,               \
      const signed char: true,        \
      const short: true,              \
      const int: true,                \
      const long: true,               \
      const long long: true,          \
      const unsigned char: true,      \
      const unsigned short: true,     \
      const unsigned int: true,       \
      const unsigned long: true,      \
      const unsigned long long: true, \
      const float: true,              \
      const double: true,             \
      const long double: true,        \
      default: false)
#endif

// Evaluates to true when x points to a type that ccol__is_integral_type()
// accepts, and to false otherwise, for code that classifies a key through a
// pointer to it. Only the type of x is read.
#if defined(CCOL__CLANG_GENERIC_ASSOC_WARNING)
#define ccol__is_integral_ptr(x)                                            \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool ccol__iip_result = _Generic((x),                                   \
        char *: true,                                                       \
        signed char *: true,                                                \
        short *: true,                                                      \
        int *: true,                                                        \
        long *: true,                                                       \
        long long *: true,                                                  \
        unsigned char *: true,                                              \
        unsigned short *: true,                                             \
        unsigned int *: true,                                               \
        unsigned long *: true,                                              \
        unsigned long long *: true,                                         \
        float *: true,                                                      \
        double *: true,                                                     \
        long double *: true,                                                \
        const char *: true,                                                 \
        const signed char *: true,                                          \
        const short *: true,                                                \
        const int *: true,                                                  \
        const long *: true,                                                 \
        const long long *: true,                                            \
        const unsigned char *: true,                                        \
        const unsigned short *: true,                                       \
        const unsigned int *: true,                                         \
        const unsigned long *: true,                                        \
        const unsigned long long *: true,                                   \
        const float *: true,                                                \
        const double *: true,                                               \
        const long double *: true,                                          \
        default: false);                                                    \
    ccol__iip_result;                                                       \
    _Pragma("GCC diagnostic pop");                                          \
  })
#else
#define ccol__is_integral_ptr(x)        \
  _Generic((x),                         \
      char *: true,                     \
      signed char *: true,              \
      short *: true,                    \
      int *: true,                      \
      long *: true,                     \
      long long *: true,                \
      unsigned char *: true,            \
      unsigned short *: true,           \
      unsigned int *: true,             \
      unsigned long *: true,            \
      unsigned long long *: true,       \
      float *: true,                    \
      double *: true,                   \
      long double *: true,              \
      const char *: true,               \
      const signed char *: true,        \
      const short *: true,              \
      const int *: true,                \
      const long *: true,               \
      const long long *: true,          \
      const unsigned char *: true,      \
      const unsigned short *: true,     \
      const unsigned int *: true,       \
      const unsigned long *: true,      \
      const unsigned long long *: true, \
      const float *: true,              \
      const double *: true,             \
      const long double *: true,        \
      default: false)
#endif

// Evaluates to true when ccol__ptr points to a signed integer type (char
// counts), const or not. cbstmap uses it to pick a signed or unsigned key
// comparison. Only the type is read.
#define ccol__is_signed_int_ptr(ccol__ptr) \
  _Generic((ccol__ptr),                    \
      char *: true,                        \
      signed char *: true,                 \
      short *: true,                       \
      int *: true,                         \
      long *: true,                        \
      long long *: true,                   \
      const char *: true,                  \
      const signed char *: true,           \
      const short *: true,                 \
      const int *: true,                   \
      const long *: true,                  \
      const long long *: true,             \
      default: false)

// Evaluates to 1 when data has an array-of-character type and 0 when it's a
// character pointer; only those two reach it, through ccol__is_char_array. It
// lets a string key be stored the right way. A pointer has the same type as its
// ccol__char_ptr_flavour() while an array doesn't, and
// __builtin_types_compatible_p() ignores top-level qualifiers, so a
// `const char *const` counts as a pointer. It's a constant expression that
// never evaluates data.
#define ccol__type_is_array_of_char(data)          \
  (!__builtin_types_compatible_p(__typeof__(data), \
                                 __typeof__(ccol__char_ptr_flavour(data))))

// ccol__is_char_ptr evaluates to true when data is a pointer to a character
// type (signed and unsigned char too), or an array of one, which _Generic
// decays. ccol__is_char_array then tells an array from a pointer; a pointer
// that is itself qualified is a pointer. The containers use both to treat every
// character pointer as a NUL-terminated string. Only the type of data is
// read.
#if defined(CCOL__CLANG_GENERIC_ASSOC_WARNING)
#define ccol__is_char_ptr(data)                                             \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool ccol__icp_result = _Generic((data),                                \
        char *: true,                                                       \
        const char *: true,                                                 \
        signed char *: true,                                                \
        const signed char *: true,                                          \
        unsigned char *: true,                                              \
        const unsigned char *: true,                                        \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    ccol__icp_result;                                                       \
  })

#define ccol__is_char_array(data) \
  (ccol__is_char_ptr((data)) && ccol__type_is_array_of_char(data))
#else
#define ccol__is_char_ptr(data)    \
  _Generic((data),                 \
      char *: true,                \
      const char *: true,          \
      signed char *: true,         \
      const signed char *: true,   \
      unsigned char *: true,       \
      const unsigned char *: true, \
      default: false)

#define ccol__is_char_array(data) \
  (ccol__is_char_ptr((data)) && ccol__type_is_array_of_char(data))
#endif

// The key and value types the containers tell apart. Every enumerator has an
// explicit value for the same reason as in ccol_retval; add a new one at the
// end with the next number. ccol_signed_char exists because the platform
// decides whether plain char is signed; a signed char * is still a ccol_string.
typedef enum ccollections_data_type {
  ccol_char = 0,
  ccol_short = 1,
  ccol_int = 2,
  ccol_long = 3,
  ccol_long_long = 4,
  ccol_unsigned_char = 5,
  ccol_unsigned_short = 6,
  ccol_unsigned_int = 7,
  ccol_unsigned_long = 8,
  ccol_unsigned_long_long = 9,
  ccol_float = 10,
  ccol_double = 11,
  ccol_long_double = 12,
  ccol_pointer = 13,
  ccol_string = 14,
  ccol_other_types = 15,
  ccol_signed_char = 16,
} ccol_data_type;

// Gives sizeof() of the C type that type names, or 0 for ccol_string,
// ccol_other_types and unknown values, which have no single width. The map
// modules use it to refuse, with ccol_invalid_args, a hand-built cmap_pair
// whose size doesn't match the declared key type, and you can use it to size
// such a pair.
static inline size_t ccol_fixed_width_data_type_size(ccol_data_type type) {
  switch (type) {
    case ccol_char:
    case ccol_signed_char:
    case ccol_unsigned_char:
      return sizeof(char);
    case ccol_short:
    case ccol_unsigned_short:
      return sizeof(short);
    case ccol_int:
    case ccol_unsigned_int:
      return sizeof(int);
    case ccol_long:
    case ccol_unsigned_long:
      return sizeof(long);
    case ccol_long_long:
    case ccol_unsigned_long_long:
      return sizeof(long long);
    case ccol_float:
      return sizeof(float);
    case ccol_double:
      return sizeof(double);
    case ccol_long_double:
      return sizeof(long double);
    case ccol_pointer:
      return sizeof(uintptr_t);
    case ccol_string:
    case ccol_other_types:
    default:
      return 0;
  }
}

// Internal: evaluates to the ccol_data_type of a scalar var, or to
// ccol_other_types for anything else. ccol_determine_ccol_data_type uses it
// after it has ruled out strings and pointers. Only the type of var is read.
#if defined(CCOL__CLANG_GENERIC_ASSOC_WARNING)
#define ccol__determine_non_special_data_type(var)                          \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    ccol_data_type ccol__dnsdt_result = _Generic((var),                     \
        char: ccol_char,                                                    \
        signed char: ccol_signed_char,                                      \
        short: ccol_short,                                                  \
        int: ccol_int,                                                      \
        long: ccol_long,                                                    \
        long long: ccol_long_long,                                          \
        unsigned char: ccol_unsigned_char,                                  \
        unsigned short: ccol_unsigned_short,                                \
        unsigned int: ccol_unsigned_int,                                    \
        unsigned long: ccol_unsigned_long,                                  \
        unsigned long long: ccol_unsigned_long_long,                        \
        float: ccol_float,                                                  \
        double: ccol_double,                                                \
        long double: ccol_long_double,                                      \
        const char: ccol_char,                                              \
        const signed char: ccol_signed_char,                                \
        const short: ccol_short,                                            \
        const int: ccol_int,                                                \
        const long: ccol_long,                                              \
        const long long: ccol_long_long,                                    \
        const unsigned char: ccol_unsigned_char,                            \
        const unsigned short: ccol_unsigned_short,                          \
        const unsigned int: ccol_unsigned_int,                              \
        const unsigned long: ccol_unsigned_long,                            \
        const unsigned long long: ccol_unsigned_long_long,                  \
        const float: ccol_float,                                            \
        const double: ccol_double,                                          \
        const long double: ccol_long_double,                                \
        default: ccol_other_types);                                         \
    _Pragma("GCC diagnostic pop");                                          \
    ccol__dnsdt_result;                                                     \
  })
#else
#define ccol__determine_non_special_data_type(var)       \
  _Generic((var),                                        \
      char: ccol_char,                                   \
      signed char: ccol_signed_char,                     \
      short: ccol_short,                                 \
      int: ccol_int,                                     \
      long: ccol_long,                                   \
      long long: ccol_long_long,                         \
      unsigned char: ccol_unsigned_char,                 \
      unsigned short: ccol_unsigned_short,               \
      unsigned int: ccol_unsigned_int,                   \
      unsigned long: ccol_unsigned_long,                 \
      unsigned long long: ccol_unsigned_long_long,       \
      float: ccol_float,                                 \
      double: ccol_double,                               \
      long double: ccol_long_double,                     \
      const char: ccol_char,                             \
      const signed char: ccol_signed_char,               \
      const short: ccol_short,                           \
      const int: ccol_int,                               \
      const long: ccol_long,                             \
      const long long: ccol_long_long,                   \
      const unsigned char: ccol_unsigned_char,           \
      const unsigned short: ccol_unsigned_short,         \
      const unsigned int: ccol_unsigned_int,             \
      const unsigned long: ccol_unsigned_long,           \
      const unsigned long long: ccol_unsigned_long_long, \
      const float: ccol_float,                           \
      const double: ccol_double,                         \
      const long double: ccol_long_double,               \
      default: ccol_other_types)
#endif

// Evaluates to the ccol_data_type of data: ccol_string for a character array or
// pointer, ccol_pointer for any other pointer, else the scalar type. chmap and
// cbmap dispatch on this key type when the typed macros create a map. The
// pointer test runs on a zeroed probe of data's type instead of data itself,
// because Clang's -Wuninitialized treats the operand of
// __builtin_classify_type() as evaluated, and data can be a companion type
// variable that is uninitialized on purpose. data is never evaluated.
#define ccol_determine_ccol_data_type(data)                               \
  ({                                                                      \
    ccol_data_type ccol__dcdt_r = ccol_other_types;                       \
    if (ccol__is_char_array((data))) {                                    \
      ccol__dcdt_r = ccol_string;                                         \
    } else if (ccol__is_char_ptr((data))) {                               \
      ccol__dcdt_r = ccol_string;                                         \
    } else {                                                              \
      __typeof__(data) ccol__dcdt_type_probe = {0};                       \
      if (__builtin_classify_type(ccol__dcdt_type_probe) == 5 &&          \
          sizeof(ccol__dcdt_type_probe) == sizeof(uintptr_t)) {           \
        ccol__dcdt_r = ccol_pointer;                                      \
      } else {                                                            \
        ccol__dcdt_r =                                                    \
            ccol__determine_non_special_data_type(ccol__dcdt_type_probe); \
      }                                                                   \
    }                                                                     \
    ccol__dcdt_r;                                                         \
  })

// Internal: evaluates to a constant 1 when lvalue has a character-pointer type,
// else 0, so __builtin_choose_expr() can select on it where the statement
// expression of ccol__is_char_ptr() can't be used. Its list must match
// ccol__is_char_ptr(). lvalue is never evaluated.
#define ccol__type_is_char_ptr(lvalue)                                      \
  (__builtin_types_compatible_p(__typeof__(lvalue), char *) ||              \
   __builtin_types_compatible_p(__typeof__(lvalue), const char *) ||        \
   __builtin_types_compatible_p(__typeof__(lvalue), signed char *) ||       \
   __builtin_types_compatible_p(__typeof__(lvalue), const signed char *) || \
   __builtin_types_compatible_p(__typeof__(lvalue), unsigned char *) ||     \
   __builtin_types_compatible_p(__typeof__(lvalue), const unsigned char *))

// Internal: evaluates to a null pointer with the character-pointer type that
// expr decays to (char * for anything else). ccol__declared_or_own_type and
// ccol__type_is_array_of_char use only its type. Every arm is a cast this
// header writes, so it's well formed for any expr in the unchosen arm of a
// __builtin_choose_expr().
#define ccol__char_ptr_flavour(expr)                   \
  _Generic((expr),                                     \
      char *: (char *)0,                               \
      const char *: (const char *)0,                   \
      signed char *: (signed char *)0,                 \
      const signed char *: (const signed char *)0,     \
      unsigned char *: (unsigned char *)0,             \
      const unsigned char *: (const unsigned char *)0, \
      default: (char *)0)

// Internal: expands to the type a container macro gives its private copy of a
// caller expression expr, so the copy has the right type and width.
// type_var is the companion type variable, so *(type_var) has the declared
// type. For a declared non-string type it's that type, so the copy converts
// expr by the usual rules and a mismatched type fails to compile. For a
// declared character-pointer type it's expr's own pointer flavour, because all
// character pointers are one string type to the library and a map keyed on
// unsigned char * must stay reachable through a char *. Arrays decay, and
// top-level qualifiers go, so ccol__clear_padding() can write the copy. expr is
// never evaluated.
#define ccol__declared_or_own_type(type_var, expr)                             \
  ccol__unqual_type(__builtin_choose_expr(ccol__type_is_char_ptr(*(type_var)), \
                                          ccol__char_ptr_flavour(expr),        \
                                          *(type_var)))

// Internal: evaluates to a constant 1 when T (a type or an expression) is an
// array type, else 0. ccol__refuse_array_types and ccol__unqual_type need it.
// The comma decays an array and keeps any other type, so the two types differ
// exactly for an array (a function type also gives 1). Nothing is evaluated.
#define ccol__type_is_array(T)                  \
  (!__builtin_types_compatible_p(__typeof__(T), \
                                 __typeof__((void)0, *(__typeof__(T) *)0)))

// Internal: refuses an array key type KeyT or value type ValT at compile time,
// with a message that starts with the container name. The typed macros copy
// through a temporary of the declared type, and an array would decay and store
// the address of the caller's array. It's two static assertions, valid inside a
// struct too, and the caller adds the semicolon.
#define ccol__refuse_array_types(container, KeyT, ValT)              \
  _Static_assert(!ccol__type_is_array(KeyT), container               \
                 ": an array key type is not supported; wrap the "   \
                 "array in a struct");                               \
  _Static_assert(!ccol__type_is_array(ValT), container               \
                 ": an array value type is not supported; wrap the " \
                 "array in a struct")

// Internal: evaluates to x (not an array) as an rvalue without top-level
// qualifiers, for ccol__unqual_type; only its type is used. Clang drops them
// with a comma, GCC before 11 doesn't, so GCC casts x to its own type (a GNU
// extension for a struct, which Clang refuses).
#if defined(__clang__)
#define ccol__unqual_nonarray_rvalue(x) ((void)0, (x))
#else
#define ccol__unqual_nonarray_rvalue(x) (__extension__(__typeof__(x))(x))
#endif

// Internal: expands to the type of x without top-level qualifiers, for a
// temporary that a macro writes to even when the declared type is const. An
// array type keeps its type, since dropping qualifiers would decay it. The
// unchosen arm sees 0 instead of x, so no cast meets an array. x is never
// evaluated.
#define ccol__unqual_type(x)        \
  __typeof__(__builtin_choose_expr( \
      ccol__type_is_array(x), (x),  \
      ccol__unqual_nonarray_rvalue( \
          __builtin_choose_expr(ccol__type_is_array(x), 0, (x)))))

// Internal: zeroes the padding bytes of the object obj_ptr points to, where the
// compiler has __builtin_clear_padding(), and does nothing elsewhere. A struct
// copy may carry stack garbage in its padding, and the containers hash and
// compare every byte of a key, so the typed macros clear their own copy of each
// key. It's an expression with no useful value.
#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
#define ccol__clear_padding(obj_ptr) __builtin_clear_padding(obj_ptr)
#endif
#endif
#ifndef ccol__clear_padding
#define ccol__clear_padding(obj_ptr) ((void)0)
#endif

// Map pair population
//
// Internal: fills *pair from data of any type, so the typed macros can hand any
// key or value to the raw cmap_pair layer. A character array gives its address
// and strlen + 1. A character pointer gives the pointer and strlen + 1, or
// {NULL, 0} for a NULL pointer, which the raw layers refuse; the pointer is
// read through __builtin_choose_expr() so no other type is ever read as a
// pointer, and through uintptr_t so the const goes without a -Wcast-qual. Any
// other type gives &data and sizeof(data); the cast to void * lets a const key
// through, and the raw layers only read it. It's a statement with no value.
#define ccol__populate_cmap_pair(pair, data)                         \
  do {                                                               \
    if (ccol__is_char_array((data))) {                               \
      (pair)->ptr = (char *)&(data);                                 \
      (pair)->size = strlen((char *)(pair)->ptr) + 1;                \
    } else if (ccol__is_char_ptr((data))) {                          \
      (pair)->ptr = (void *)(uintptr_t)__builtin_choose_expr(        \
          ccol__type_is_char_ptr(data), (data), (const char *)0);    \
      (pair)->size =                                                 \
          (pair)->ptr ? strlen((char *)(pair)->ptr) + 1 : (size_t)0; \
    } else {                                                         \
      (pair)->ptr = (void *)&(data);                                 \
      (pair)->size = sizeof((data));                                 \
    }                                                                \
  } while (0)

// Utility macros
//
// Internal: evaluate to the smaller or larger of a and b, for the size
// arithmetic of the library and its inline code. They can evaluate an argument
// twice.
#define ccol__min(a, b) ((a) < (b) ? (a) : (b))

#define ccol__max(a, b) ((a) > (b) ? (a) : (b))

// Internal: compares the two T values at ptr1 and ptr2 and evaluates to -1, 0
// or 1 (negative, zero or positive in general). cbstmap and the default
// comparators use it. Both are read with memcpy, because the bytes can come
// from a hand-built cmap_pair with no alignment and no effective type of T, and
// a load through a T * would be undefined. A constant-size memcpy compiles to
// the same single load.
#define ccol__typed_cmp(ptr1, ptr2, T)                                     \
  ({                                                                       \
    T ccol__tcmp_lhs;                                                      \
    T ccol__tcmp_rhs;                                                      \
    memcpy(&ccol__tcmp_lhs, (ptr1), sizeof(T));                            \
    memcpy(&ccol__tcmp_rhs, (ptr2), sizeof(T));                            \
    (ccol__tcmp_lhs > ccol__tcmp_rhs) - (ccol__tcmp_lhs < ccol__tcmp_rhs); \
  })

#pragma GCC visibility pop
