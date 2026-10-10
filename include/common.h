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

/**
 * @file common.h
 * @brief Common definitions, types, and utilities for the C collections library
 *
 * It gives the base infrastructure that all the collection types use:
 * - Thread primitives (mutex, rwlock, condition variables, one-time
 *   initialization, thread creation and join, thread-local storage keys,
 *   fork handler registration)
 * - Macros for error handling and error reports
 * - The memory management abstraction layer
 * - Return value codes
 * - Type introspection with C11 _Generic
 * - Map key-value pair structures
 * - Types for custom comparison and hash functions
 *
 * Every collection in this library includes this header. It gives one
 * consistent interface for thread safety, memory management, and error
 * handling.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every declaration from here to the end of this header is part of the
 * public ABI of libccollections. The shared library exports all of them. The
 * build of the library uses -fvisibility=hidden. A function or an object that
 * no such block covers stays internal to the library. It is absent from the
 * dynamic symbol table. No symbol of the same name in the application that
 * links against the library can interpose it or collide with it. */
#pragma GCC visibility push(default)

/* ========================================================================== */
/*                         FORK SAFETY OPT-OUT                                */
/* ========================================================================== */

/**
 * @brief A compile-time switch that decides whether the build compiles the
 *        fork() safety machinery of this library. That machinery uses
 *        pthread_atfork().
 *
 * Some modules register pthread_atfork() prepare, parent and child handlers.
 * These modules are cthreadpool, the ccol_event_loop and the queue types of
 * cthreadcomm, clogger, chttpserver and chttpclient. A fork() can happen while
 * a thread holds one of their internal locks. The handlers make sure that the
 * child does not get a mutex that stays locked forever. The child has no thread
 * left alive that could unlock such a mutex.
 *
 * That protection costs real work on every fork() call in the process, from
 * any thread, for any reason. The prepare handler walks the process-wide
 * registry of its own module. It must lock the internal lock of every live
 * handle before fork() can continue. The parent handler and the child handler
 * must then unlock all of them again.
 *
 * Some applications never call fork(). Others always call exec() immediately
 * after fork(), to run a subprocess with fork() and execve(), or with an
 * equivalent posix_spawn() wrapper. The child of such a fork never touches a
 * handle of this library before it replaces its own process image. These
 * applications get no benefit from the protection. They can define this
 * switch to 0, for example with -DCCOL_FORK_SAFETY_REQUIRED=0.
 *
 * The build then removes every one of these registrations from the library.
 * It also removes the bodies of the handlers, and the state in a handle that
 * exists only to support them. One example of that state is a flag that says
 * "a fork() gave this handle to a child". The cost for each fork() goes away
 * with them.
 *
 * This switch changes nothing else about the thread safety of a module. Its
 * own locks stay. The close-on-exec gate below also stays, because it keeps
 * the descriptors of the library out of a child that calls exec(), which is
 * the pattern that this switch is for. The safety of a concurrent create and
 * destroy through the generation-tagged handle tables stays. The switch
 * controls only the protection that is specific to fork().
 *
 * The value is 1, which compiles the fork safety in. This is the value unless
 * a caller defines the macro before the first include of this header.
 */
#ifndef CCOL_FORK_SAFETY_REQUIRED
#define CCOL_FORK_SAFETY_REQUIRED 1
#endif

/* ========================================================================== */
/*                         THREADING PRIMITIVES                               */
/* ========================================================================== */

/* macOS has no pthread_mutex_timedlock(), no pthread_rwlock_timedwrlock(), no
 * pthread_condattr_setclock() and no working sem_init(). The wrappers below
 * give it each of them: a timed lock polls a try-lock within its deadline, a
 * condition variable records its clock and turns a monotonic deadline into a
 * relative wait, and a semaphore is a pipe. _CCOL_EMULATE_DARWIN_SYNC selects
 * the same code on any other system, which is how the test suites of Linux
 * and FreeBSD run it; there the relative wait is the absolute one of the
 * realtime clock. Only the library defines it, never an application. */
#if defined(__APPLE__) && !defined(_CCOL_EMULATE_DARWIN_SYNC)
#define _CCOL_EMULATE_DARWIN_SYNC 1
#endif

/* The close-on-exec gate. Where a descriptor cannot be created closed on
 * exec in one call (a socket, an accepted socket, a socket pair and a pipe on
 * macOS), the library creates it and then sets FD_CLOEXEC. A fork() of
 * another thread between the two steps gives the child the descriptor without
 * the flag, and an exec() in that child keeps it open. The library therefore
 * makes both steps between _ccol_cloexec_gate_enter() and
 * _ccol_cloexec_gate_leave(), which take the read side of a process-wide
 * lock, and a fork-prepare handler takes the write side. A fork() thus waits
 * until no descriptor is between the two steps. posix_spawn(3) runs no fork
 * handler, so a program that spawns a child on macOS while the library
 * creates a socket uses POSIX_SPAWN_CLOEXEC_DEFAULT. The gate exists only
 * where the two steps exist, so it costs nothing on Linux and FreeBSD. The
 * two functions are internal and not exported. */
#if defined(__APPLE__) || defined(_CCOL_EMULATE_DARWIN_SYNC) || \
    defined(_CCOL_EMULATE_DARWIN_SOCK)
#define _CCOL_CLOEXEC_GATE 1
#pragma GCC visibility push(hidden)
void _ccol_cloexec_gate_enter(void);
void _ccol_cloexec_gate_leave(void);
void _ccol_cloexec_gate_register(void);
#pragma GCC visibility pop
#endif

/** @brief Mutex type (wraps pthread_mutex_t) */
#define ccol_mutex_t pthread_mutex_t

/** @brief Destroy a mutex */
#define ccol_mutex_destroy(m) pthread_mutex_destroy(&(m))

/** @brief Initialize a mutex with default attributes */
#define ccol_mutex_init(m) pthread_mutex_init(&(m), NULL)

/** @brief Lock a mutex. The call blocks. */
#define ccol_mutex_lock(m) pthread_mutex_lock(&(m))

/** @brief Unlock a mutex */
#define ccol_mutex_unlock(m) pthread_mutex_unlock(&(m))

/**
 * @brief Lock a mutex, or give up at an absolute CLOCK_REALTIME time t (a
 *        struct timespec). It returns 0 or ETIMEDOUT, as the wrapped call
 *        does.
 */
#if defined(_CCOL_EMULATE_DARWIN_SYNC)
#define ccol_mutex_timedlock(m, t) _ccol_mutex_timedlock_poll(&(m), &(t))
#else
#define ccol_mutex_timedlock(m, t) pthread_mutex_timedlock(&(m), &(t))
#endif

/** @brief Read-write lock type (wraps pthread_rwlock_t) */
#define ccol_rw_lock_t pthread_rwlock_t

/** @brief Destroy a read-write lock */
#define ccol_rw_lock_destroy(a) pthread_rwlock_destroy(&(a))

/** @brief Initialize a read-write lock with default attributes */
#define ccol_rw_lock_init(a) pthread_rwlock_init(&(a), NULL)

/**
 * @brief Initialize again, in the child of fork(), a read-write lock that the
 *        forking thread held
 *
 * The child has one thread, so nothing waits on the lock, but a plain unlock
 * does not always release it there (glibc compares the thread ID of the
 * writer, and the child's thread has a new one). macOS refuses to initialize
 * an object that it still sees as a lock in use, with EBUSY. The object is
 * therefore cleared first. On FreeBSD the lock is a pointer to a block that
 * the init replaces in either case.
 */
#define ccol_rw_lock_reinit_in_child(a) \
  (memset(&(a), 0, sizeof(a)), pthread_rwlock_init(&(a), NULL))

/** @brief Take the write lock (exclusive access) */
#define ccol_rw_lock_wrlock(a) pthread_rwlock_wrlock(&(a))

/** @brief Take the read lock (shared access) */
#define ccol_rw_lock_rdlock(a) pthread_rwlock_rdlock(&(a))

/**
 * @brief Take the write lock, or give up at an absolute CLOCK_REALTIME time t
 *        (a struct timespec). It returns 0 or ETIMEDOUT, as the wrapped call
 *        does.
 */
#if defined(_CCOL_EMULATE_DARWIN_SYNC)
#define ccol_rw_lock_timedwrlock(a, t) \
  _ccol_rw_lock_timedwrlock_poll(&(a), &(t))
#else
#define ccol_rw_lock_timedwrlock(a, t) pthread_rwlock_timedwrlock(&(a), &(t))
#endif

/** @brief Unlock a read-write lock */
#define ccol_rw_lock_unlock(a) pthread_rwlock_unlock(&(a))

#if !defined(_CCOL_EMULATE_DARWIN_SYNC)
/** @brief Condition variable type (wraps pthread_cond_t) */
#define ccol_cond_var_t pthread_cond_t

/** @brief Condition variable attributes type (wraps pthread_cond_attr_t) */
#define ccol_cond_var_attr_t pthread_condattr_t

/** @brief Destroy condition variable attributes */
#define ccol_cond_var_attr_destroy(ca) pthread_condattr_destroy(&(ca))

/** @brief Initialize condition variable attributes */
#define ccol_cond_var_attr_init(ca) pthread_condattr_init(&(ca))

/** @brief Set the clock type of condition variable attributes */
#define ccol_cond_var_attr_setclock(ca, clk) \
  pthread_condattr_setclock(&(ca), clk)

/** @brief Destroy a condition variable */
#define ccol_cond_var_destroy(c) pthread_cond_destroy(&(c))

/** @brief Initialize a condition variable with default attributes */
#define ccol_cond_var_init(c) pthread_cond_init(&(c), NULL)

/** @brief Initialize a condition variable with custom attributes */
#define ccol_cond_var_init_ca(c, ca) pthread_cond_init(&(c), &(ca))

/** @brief Wait on a condition variable. The wait unlocks the mutex. */
#define ccol_cond_var_wait(c, m) pthread_cond_wait(&(c), &(m))

/** @brief A timed wait on a condition variable, with an absolute timeout */
#define ccol_cond_var_timedwait(c, m, t) \
  pthread_cond_timedwait(&(c), &(m), &(t))

/** @brief Signal one thread that waits on a condition variable */
#define ccol_cond_var_signal(c) pthread_cond_signal(&(c))

/** @brief Signal every thread that waits on a condition variable */
#define ccol_cond_var_broadcast(c) pthread_cond_broadcast(&(c))
#else
/* A condition variable that records the clock of its deadlines; see
 * _CCOL_EMULATE_DARWIN_SYNC above. */
typedef struct {
  pthread_cond_t cond;
  clockid_t clock;
} _ccol_cond_var_s;
typedef struct {
  clockid_t clock;
} _ccol_cond_var_attr_s;

static inline int _ccol_cond_var_attr_set(_ccol_cond_var_attr_s *ca,
                                          clockid_t clk) {
  ca->clock = clk;
  return 0;
}
static inline int _ccol_cond_var_init_clock(_ccol_cond_var_s *c,
                                            clockid_t clk) {
  c->clock = clk;
  return pthread_cond_init(&c->cond, NULL);
}

#define ccol_cond_var_t _ccol_cond_var_s
#define ccol_cond_var_attr_t _ccol_cond_var_attr_s
#define ccol_cond_var_attr_destroy(ca) \
  _ccol_cond_var_attr_set(&(ca), (ca).clock)
#define ccol_cond_var_attr_init(ca) \
  _ccol_cond_var_attr_set(&(ca), CLOCK_REALTIME)
#define ccol_cond_var_attr_setclock(ca, clk) \
  _ccol_cond_var_attr_set(&(ca), (clk))
#define ccol_cond_var_destroy(c) pthread_cond_destroy(&(c).cond)
#define ccol_cond_var_init(c) _ccol_cond_var_init_clock(&(c), CLOCK_REALTIME)
#define ccol_cond_var_init_ca(c, ca) _ccol_cond_var_init_clock(&(c), (ca).clock)
#define ccol_cond_var_wait(c, m) pthread_cond_wait(&(c).cond, &(m))
#define ccol_cond_var_timedwait(c, m, t) \
  _ccol_cond_var_timedwait_clock(&(c), &(m), &(t))
#define ccol_cond_var_signal(c) pthread_cond_signal(&(c).cond)
#define ccol_cond_var_broadcast(c) pthread_cond_broadcast(&(c).cond)
#endif

/**
 * @brief Unnamed, process-private semaphore type (wraps sem_t)
 *
 * This is the one synchronization primitive in this file that POSIX
 * guarantees to be async-signal-safe. The safe operation is
 * ccol_semaphore_post() below, which uses sem_post(3). Every other primitive
 * here is NOT safe to call inside a signal handler. That covers ccol_mutex_t,
 * ccol_cond_var_t, ccol_rw_lock_t, ccol_thread_create() and ccol_call_once().
 *
 * pthread_mutex_lock can deadlock against itself. This happens when the
 * interrupted thread already holds the exact mutex that the handler tries to
 * lock. pthread_create and pthread_once can need the arena lock of malloc.
 * The interrupted thread can already hold that lock for an unrelated reason.
 *
 * A module can need an entry point that is truly async-signal-safe, for
 * example one that is "safe to call from a SIGTERM handler". Do not call
 * ccol_mutex_lock, ccol_thread_create or ccol_call_once from such an entry
 * point. Wake a dedicated watcher thread that already runs, with
 * ccol_semaphore_post(). That thread is an ordinary execution context, and
 * not a signal context, so it does the real work. The engine-stop watcher in
 * chttpserver.c is the reference implementation of this pattern.
 */
#if !defined(_CCOL_EMULATE_DARWIN_SYNC)
#define ccol_semaphore_t sem_t

/** @brief Initialize an unnamed, process-private semaphore. Its count
 *  starts at value. */
#define ccol_semaphore_init(s, value) sem_init(&(s), 0, (value))

/** @brief Destroy a semaphore initialized with ccol_semaphore_init(). */
#define ccol_semaphore_destroy(s) sem_destroy(&(s))

/** @brief Block until the count of the semaphore is > 0, then decrement it
 *  atomically. This macro is not async-signal-safe, because it can block.
 *  Call it only from an ordinary thread, and never inside a signal
 *  handler. */
#define ccol_semaphore_wait(s) sem_wait(&(s))

/**
 * @brief Increment the count of the semaphore. This wakes one waiter, if
 *        there is one.
 *
 * POSIX makes sem_post(3) async-signal-safe. This is the one operation in
 * this file that is safe to call inside a signal handler.
 */
#define ccol_semaphore_post(s) sem_post(&(s))
#else
/* A semaphore made of a pipe: each byte in the pipe is one unit of the
 * count. write(2) is async-signal-safe, so a post keeps the guarantee that
 * the semaphore exists for. */
typedef struct {
  int fd[2];
} _ccol_semaphore_s;
#define ccol_semaphore_t _ccol_semaphore_s
#define ccol_semaphore_init(s, value) _ccol_semaphore_pipe_init(&(s), (value))
#define ccol_semaphore_destroy(s) _ccol_semaphore_pipe_destroy(&(s))
#define ccol_semaphore_wait(s) _ccol_semaphore_pipe_wait(&(s))
#define ccol_semaphore_post(s) _ccol_semaphore_pipe_post(&(s))
#endif

#if defined(_CCOL_EMULATE_DARWIN_SYNC)
#include <fcntl.h>

/* True once the clock clk has reached the absolute time t. */
static inline bool _ccol_time_reached(clockid_t clk, const struct timespec *t) {
  struct timespec now;
  clock_gettime(clk, &now);
  return now.tv_sec > t->tv_sec ||
         (now.tv_sec == t->tv_sec && now.tv_nsec >= t->tv_nsec);
}

/* One pause of a try-lock loop: 20 microseconds at first, doubling to 1
 * millisecond, so a short wait costs little and a long one little CPU. */
static inline void _ccol_try_lock_pause(unsigned *us) {
  *us = *us ? (*us >= 500u ? 1000u : *us * 2u) : 20u;
  struct timespec ts = {0, (long)*us * 1000L};
  nanosleep(&ts, NULL);
}

/* pthread_mutex_timedlock() with t on CLOCK_REALTIME. */
static inline int _ccol_mutex_timedlock_poll(pthread_mutex_t *m,
                                             const struct timespec *t) {
  unsigned us = 0;
  for (;;) {
    int rc = pthread_mutex_trylock(m);
    if (rc != EBUSY) return rc;
    if (_ccol_time_reached(CLOCK_REALTIME, t)) return ETIMEDOUT;
    _ccol_try_lock_pause(&us);
  }
}

/* pthread_rwlock_timedwrlock() with t on CLOCK_REALTIME. */
static inline int _ccol_rw_lock_timedwrlock_poll(pthread_rwlock_t *a,
                                                 const struct timespec *t) {
  unsigned us = 0;
  for (;;) {
    int rc = pthread_rwlock_trywrlock(a);
    if (rc != EBUSY) return rc;
    if (_ccol_time_reached(CLOCK_REALTIME, t)) return ETIMEDOUT;
    _ccol_try_lock_pause(&us);
  }
}

/* A timed wait whose deadline t is on the clock that c records. A monotonic
 * deadline becomes the time left, which macOS waits for with
 * pthread_cond_timedwait_relative_np(); elsewhere (a test build) the time
 * left is added to the realtime clock. It gives 0 or ETIMEDOUT. */
static inline int _ccol_cond_var_timedwait_clock(_ccol_cond_var_s *c,
                                                 pthread_mutex_t *m,
                                                 const struct timespec *t) {
  if (c->clock != CLOCK_MONOTONIC)
    return pthread_cond_timedwait(&c->cond, m, t);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  /* The time left, field by field: a deadline at the end of time (a
   * saturated one) would overflow a count of nanoseconds. A wait longer than
   * 10^9 seconds is cut to that; every caller waits in a loop that checks
   * its condition, so an early wake only waits again. */
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
  at.tv_sec += rel.tv_sec;
  at.tv_nsec += rel.tv_nsec;
  if (at.tv_nsec >= 1000000000L) {
    at.tv_sec++;
    at.tv_nsec -= 1000000000L;
  }
  return pthread_cond_timedwait(&c->cond, m, &at);
#endif
}

/* sem_init(): a pipe that holds value bytes. It gives 0, or -1 with errno
 * set, as sem_init() does. */
static inline int _ccol_semaphore_pipe_init(_ccol_semaphore_s *s,
                                            unsigned value) {
  _ccol_cloexec_gate_enter();
  if (pipe(s->fd) != 0) {
    int saved = errno;
    _ccol_cloexec_gate_leave();
    errno = saved;
    return -1;
  }
  (void)fcntl(s->fd[0], F_SETFD, FD_CLOEXEC);
  (void)fcntl(s->fd[1], F_SETFD, FD_CLOEXEC);
  _ccol_cloexec_gate_leave();
  for (unsigned i = 0; i < value; i++) {
    if (write(s->fd[1], "", 1) != 1) {
      int saved = errno;
      close(s->fd[0]);
      close(s->fd[1]);
      errno = saved;
      return -1;
    }
  }
  return 0;
}

static inline int _ccol_semaphore_pipe_destroy(_ccol_semaphore_s *s) {
  close(s->fd[0]);
  close(s->fd[1]);
  return 0;
}

static inline int _ccol_semaphore_pipe_wait(_ccol_semaphore_s *s) {
  char b;
  for (;;) {
    ssize_t n = read(s->fd[0], &b, 1);
    if (n == 1) return 0;
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
}

/* write(2) is async-signal-safe, so this is too. */
static inline int _ccol_semaphore_pipe_post(_ccol_semaphore_s *s) {
  for (;;) {
    ssize_t n = write(s->fd[1], "", 1);
    if (n == 1) return 0;
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
}
#endif /* _CCOL_EMULATE_DARWIN_SYNC */

/** @brief Thread ID type (wraps pthread_t) */
#define ccol_thread_id_t pthread_t

/** @brief Get current thread ID */
#define ccol_get_thread_id pthread_self

/** @brief Nonzero when two thread IDs name the same thread (wraps
 *         pthread_equal) */
#define ccol_thread_id_equal(a, b) pthread_equal((a), (b))

/**
 * @brief Create a thread that runs fn(arg), and store its handle in handle
 *
 * Every call site passes the default attributes, which are NULL. Each one
 * reads the return value only as a success or a failure. This is why the
 * wrapper hides the attributes argument. ccol_mutex_init() and
 * ccol_cond_var_init() already hide their own NULL attributes argument in the
 * same way.
 */
#define ccol_thread_create(handle, fn, arg) \
  pthread_create(&(handle), NULL, (fn), (arg))

/** @brief Block until the thread that handle identifies stops */
#define ccol_thread_join(handle) pthread_join((handle), NULL)

/**
 * @brief Examine or change the signal mask of the calling thread (wraps
 *        pthread_sigmask)
 *
 * how is SIG_BLOCK, SIG_UNBLOCK or SIG_SETMASK, and set and oldset are
 * sigset_t pointers, either of which can be NULL, exactly as for
 * pthread_sigmask(). It gives 0 on success and an error number on a failure.
 * The translation unit that uses it includes <signal.h>.
 */
#define ccol_thread_sigmask(how, set, oldset) \
  pthread_sigmask((how), (set), (oldset))

/** @brief One-time-initialization guard type (wraps pthread_once_t) */
#define ccol_once_flag_t pthread_once_t

/**
 * @brief Static initializer for a ccol_once_flag_t (wraps PTHREAD_ONCE_INIT)
 *
 * A one-time-init flag holds a simple "not yet run" state that every platform
 * can represent. It is not a real synchronization object, and a mutex, a
 * condition variable and a rwlock are. This is why it is the one primitive in
 * this file that can still use a static, constant initializer. It is also
 * what lets the library initialize everything else lazily, with no
 * chicken-and-egg problem.
 */
#define CCOL_ONCE_INIT PTHREAD_ONCE_INIT

/** @brief Run fn exactly one time for all the callers that race on the
 *  same flag */
#define ccol_call_once(flag, fn) pthread_once(&(flag), (fn))

/** @brief Thread-local storage key type (wraps pthread_key_t) */
#define ccol_thread_ls_key_t pthread_key_t

/** @brief Create a thread-local storage key with an optional destructor */
#define ccol_thread_ls_key_create(key, destructor) \
  pthread_key_create(&(key), (destructor))

/** @brief Delete a thread-local storage key */
#define ccol_thread_ls_key_delete(key) pthread_key_delete((key))

/** @brief Set the calling thread's value for a thread-local storage key */
#define ccol_thread_ls_set(key, val) pthread_setspecific((key), (val))

/** @brief Get the calling thread's value for a thread-local storage key */
#define ccol_thread_ls_get(key) pthread_getspecific((key))

/** @brief Register prepare, parent and child handlers that run around
 *  fork(2)
 *
 * Where the close-on-exec gate exists, this registers the handlers of the
 * gate first, one time. The prepare handlers run in the reverse order of
 * their registration, so the gate then waits after every other prepare
 * handler of the library, whichever of them registers first. */
#define _ccol_at_fork_raw(prepare, parent, child) \
  pthread_atfork((prepare), (parent), (child))
#if defined(_CCOL_CLOEXEC_GATE)
#define ccol_at_fork(prepare, parent, child) \
  (_ccol_cloexec_gate_register(), _ccol_at_fork_raw(prepare, parent, child))
#else
#define ccol_at_fork(prepare, parent, child) \
  _ccol_at_fork_raw(prepare, parent, child)
#endif

/**
 * @brief Get a thread's name (wraps pthread_getname_np)
 *
 * This is not portable, because only Apple and BSD have it. Only the
 * platform fallback branches in clogger.c use it. Those branches serve a
 * system that has no thread name lookup through /proc.
 */
#define ccol_get_thread_name_np(thread, buf, len) \
  pthread_getname_np((thread), (buf), (len))

/**
 * @brief Rename the calling thread (wraps pthread_setname_np)
 *
 * Linux renames a thread through prctl(PR_SET_NAME) instead, so only the
 * platform branches of ccol_set_thread_name() in clogger.c use this. macOS
 * can rename only the calling thread, and its pthread_setname_np takes the
 * name alone; FreeBSD takes the thread too.
 */
#if defined(__APPLE__)
#define ccol_set_own_thread_name_np(name) pthread_setname_np((name))
#else
#define ccol_set_own_thread_name_np(name) \
  pthread_setname_np(pthread_self(), (name))
#endif

/**
 * @brief Get a thread's 64-bit numeric id (wraps pthread_threadid_np)
 *
 * This is not portable, because only Apple has it. Only the platform
 * fallback branch in clogger.c uses it. That branch serves a system that has
 * no thread id lookup through a syscall.
 */
#define ccol_get_thread_id_np(thread, out_id) \
  pthread_threadid_np((thread), (out_id))

/**
 * @brief Assertion macro for collections library
 *
 * @param cond Condition that must hold
 *
 * This macro does not use the assert() of the standard library, on purpose.
 * The library documents a "will assert on misuse" contract in many places.
 * That contract covers double-free detection and corruption detection. It
 * also covers the rejection of a foreign pointer, and of a pointer from
 * another container. It covers every other invariant that this macro
 * guards in every module. These are
 * load-bearing safety checks, and a caller can depend on them. They are not
 * debug-only checks for a release build to compile away.
 *
 * A plain assert() quietly becomes a no-op when NDEBUG is defined. Every one
 * of those checks then stops the abort. The program continues on a state that
 * is known to be invalid. One example is a dereference of a null pointer one
 * line later. Another is a double free that nobody catches. These are the
 * exact outcomes that the checks exist to prevent.
 *
 * This macro always evaluates cond. It always stops the program with abort()
 * when cond is false. abort() raises SIGABRT, which is the signal that a
 * failure of a standard assert() also raises. NDEBUG and every other
 * compilation setting change none of this.
 */
#define ccol_assert(cond)                                                    \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "%s:%d: ccol_assert failed: %s\n", __FILE__, __LINE__, \
              #cond);                                                        \
      abort();                                                               \
    }                                                                        \
  } while (0)

/* ========================================================================== */
/*                         ERROR HANDLING                                     */
/* ========================================================================== */

/**
 * @brief Convert argument to string literal (internal helper)
 * @param s Argument to stringify
 */
#define ccol_stringify(s) #s

/**
 * @brief Expand and stringify macro argument
 * @param s Macro to expand then stringify
 */
#define ccol_x_stringify(s) ccol_stringify(s)

/* Internal. Gives an identifier that no other expansion in the translation
 * unit can give: prefix, an underscore, and the value of n. The container
 * macros pass __COUNTER__ as n and name every one of their temporaries this
 * way. A temporary is in scope inside its own initializer, and that
 * initializer is where the argument of the caller is evaluated, so a public
 * macro nested in the argument of another public macro, or of itself (such
 * as chmap_get(m, chmap_get(m, k))), would otherwise declare a second local
 * of the same name there. -Wshadow reports that, and a -Werror build of the
 * caller then fails. The two levels make n expand before the paste. */
#define _ccol_uniq(prefix, n) _ccol_uniq_paste(prefix, n)
#define _ccol_uniq_paste(prefix, n) prefix##_##n

/* Internal. One _Generic association, with its trailing comma, that maps the
 * type of the C23 nullptr constant to v, in a translation unit that has
 * nullptr; nothing otherwise. A classifying _Generic writes it among its
 * associations, so that nullptr selects the same result as a NULL of type
 * void *. GCC 13 and Clang 16 are the first releases with nullptr, and they
 * report a __STDC_VERSION__ below 202311L for -std=c2x. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L && \
    ((defined(__clang__) && __clang_major__ >= 16) ||          \
     (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 13))
#define _CCOL_NULLPTR_ASSOC(v) __typeof__(nullptr) : v,
#else
#define _CCOL_NULLPTR_ASSOC(v)
#endif

/**
 * @brief The ownership rule for every `err` and `err_str` out-parameter in
 *        this library.
 *
 * THE LIBRARY OWNS THE STRING. A CALLER MUST NEVER FREE IT.
 *
 * One rule covers every module. Most of them hand back a CCOL_ERR_STR()
 * literal, which has static storage duration and lives for the whole
 * process. cjson and cyaml build a message that names a position and a
 * token, so their text is not known until the parse fails; they hand back a
 * pointer into a per-thread buffer that the module owns instead. Both kinds
 * are library storage, and free() is wrong for both.
 *
 * The lifetime differs between the two, and only the second kind has one
 * worth naming. A literal is valid forever. A buffered message is valid
 * until the next FAILING call of the same kind on the SAME thread, which is
 * the lifetime that strerror(3) and dlerror(3) give. Copy the text if you
 * need it past that point. The doc comment of each such function says so
 * again at the point of use.
 *
 * An out-parameter that a caller DOES own is never called err. It is always
 * a return value or an explicitly named buffer, and its own doc comment
 * names the function that frees it.
 */

/**
 * @brief Create error string with file and line information
 *
 * Makes a compile-time error string that holds the source file, the line
 * number, and a custom error message.
 *
 * @param x Error message string
 * @return String literal: "file:line - message"
 *
 * Example:
 * @code
 * char *err = CCOL_ERR_STR("failed to allocate memory");
 * // Results in: "myfile.c:42 - failed to allocate memory"
 * @endcode
 */
#define CCOL_ERR_STR(x) (__FILE__ ":" ccol_x_stringify(__LINE__) " - " x)

/**
 * @brief Fatal error macro - print message and assert
 *
 * This macro formats an error message to stderr. It then causes an assertion
 * failure. Use it for an error that the program cannot recover from.
 *
 * @param _err_fmt Printf-style format string
 * @param ... Format arguments
 *
 * @note This macro always stops the program with ccol_assert(false)
 * @note The macro writes the message with one fprintf() call to stderr,
 * prefixed with the file and the line, so the message has no length limit
 *
 * Example:
 * @code
 * if (!ptr) {
 *   ccol_fatal_err("allocation failed: size=%zu", requested_size);
 * }
 * @endcode
 */
#define ccol_fatal_err(...) _ccol_fatal_err_impl(__VA_ARGS__, "")

/* The public macro appends one empty string to the arguments of the caller,
 * and the trailing %s of the format prints it. The list after the format
 * string is therefore never empty, so the macro needs neither the GNU comma
 * swallow (, ##__VA_ARGS__) nor at least one argument after the format. Both
 * are extensions that clang -pedantic and gcc -pedantic reject under
 * -std=c11, and the inline functions of the installed headers expand this
 * macro, so a strict build that merely includes them would fail. */
#define _ccol_fatal_err_impl(_err_fmt, ...)                               \
  do {                                                                    \
    fprintf(stderr, "%s:%d: fatal: " _err_fmt "%s\n", __FILE__, __LINE__, \
            __VA_ARGS__);                                                 \
    ccol_assert(false);                                                   \
  } while (0)

/* ========================================================================== */
/*                         MEMORY MANAGEMENT                                  */
/* ========================================================================== */

/**
 * @brief Allocate memory (default: malloc)
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
#define ccol_mem_alloc(size) malloc((size))

/**
 * @brief Allocate and zero-initialize memory (default: calloc)
 * @param elem_count Number of elements
 * @param elem_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
#define ccol_mem_calloc(elem_count, elem_size) calloc((elem_count), (elem_size))

/**
 * @brief Reallocate memory (default: realloc)
 * @param ptr Existing pointer to reallocate
 * @param new_size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
#define ccol_mem_realloc(ptr, new_size) realloc((ptr), (new_size))

/**
 * @brief Free memory (default: free)
 * @param ptr Pointer to free
 */
#define ccol_mem_free(ptr) free((ptr))

/**
 * @brief Allocate memory with the custom allocator or the default one
 * @param m_procs Memory management procedures (or NULL for default)
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
/* The outer parentheses are load-bearing. ?: binds more loosely than every
 * arithmetic operator, relational operator and cast operator. A body with no
 * parentheses lets an operator outside the macro bind to one arm only.
 * Without them, `(char *)_ccol_mem_alloc(mp, n) + off` parses as
 * `(char *)(mp) ? (mp)->malloc(n) : malloc(n) + off`. That applies the cast
 * to m_procs, and the offset to the arm that the code does not take. Both
 * arms are void *, and -Wpointer-arith is in neither -Wall nor -Wextra. This
 * is why the wrong code compiles with no report. The same is true for every
 * sibling macro below. */
#define _ccol_mem_alloc(m_procs, size) \
  ((m_procs) ? (m_procs)->malloc((size)) : ccol_mem_alloc((size)))

/**
 * @brief Allocate zeroed memory with the custom allocator or the default
 *        one
 * @param m_procs Memory management procedures (or NULL for default)
 * @param e_count Number of elements
 * @param e_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
#define _ccol_mem_calloc(m_procs, e_count, e_size)    \
  ((m_procs) ? (m_procs)->calloc((e_count), (e_size)) \
             : ccol_mem_calloc((e_count), (e_size)))

/**
 * @brief Reallocate memory with the custom allocator or the default one
 * @param m_procs Memory management procedures (or NULL for default)
 * @param ptr Existing pointer
 * @param new_size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
#define _ccol_mem_realloc(m_procs, ptr, new_size)    \
  ((m_procs) ? (m_procs)->realloc((ptr), (new_size)) \
             : ccol_mem_realloc((ptr), (new_size)))

/**
 * @brief Free memory with the custom allocator or the default one
 * @param m_procs Memory management procedures (or NULL for default)
 * @param ptr Pointer to free
 */
#define _ccol_mem_free(m_procs, ptr) \
  ((m_procs) ? (m_procs)->free((ptr)) : ccol_mem_free((ptr)))

/**
 * @brief The invalid size_t for operations that use a size
 *
 * This is the maximum value that a size_t can hold
 */
#define ccol_invalid_size ((size_t)-1)

/**
 * @brief The maximum power of two that can be stored in a size_t
 *
 * on 32 bit archs -> 2^31
 *
 * on 64 bit archs -> 2^63
 *
 * 2^(#arch_bits) is larger than the storage of a size_t. This is why the
 * largest integer power of two that a size_t variable can hold is half of
 * 2^(#arch_bits).
 */
#define ccol_max_power_of_two_size_t \
  ((size_t)1 << ((sizeof(size_t) * CHAR_BIT) - 1))

/**
 * @brief Maximum element count for collections
 *
 * This is the maximum power of 2 that a size_t can hold
 */
#define ccol_max_elem_count ccol_max_power_of_two_size_t

/* ========================================================================== */
/*                         RETURN VALUE CODES                                 */
/* ========================================================================== */

/**
 * @brief Standard return codes for collection operations
 *
 * Every collection function returns one of these codes. The code shows a
 * success, or it shows the exact type of the failure. A negative value shows
 * an error. Zero shows a success.
 */
typedef enum ccollections_retval_t {
  /* Every enumerator below has an explicit value, and this is deliberate. C
   * gives the same values automatically when they are implicit.
   * ccol_success == 0 is a load-bearing invariant, and real call sites depend
   * on it. Some of them compare "== 0" directly, and do not write
   * ccol_success. A list with implicit values renumbers every later entry
   * quietly, and that includes ccol_success itself. This happens as soon as
   * somebody inserts a new enumerator anywhere except at the very end.
   *
   * Such a renumbering is invisible at the point of the edit. It appears far
   * away from that point, as a test of an unrelated module that fails on a
   * retval that it never touched. Pin every value explicitly, so a future
   * addition cannot cause that. */
  ccol_unexpected_failure = -18,          /**< Unexpected/unknown error */
  ccol_http_connection_failed = -17,      /**< TCP connection to the server
                                             could not be established */
  ccol_http_host_resolution_failed = -16, /**< DNS or hostname resolution
                                             failed */
  ccol_http_tls_handshake_failed = -15,   /**< TLS/SSL handshake with the server
                                            failed */
  ccol_http_tls_cert_verification_failed = -14, /**< Peer TLS certificate
                                                   could not be verified */
  ccol_http_tls_cert_load_failed = -13, /**< A local certificate/key/CA-bundle
                                          file could not be read or was
                                          malformed */
  ccol_http_too_many_redirects = -12,   /**< HTTP redirect limit was exceeded */
  ccol_http_invalid_url = -11,      /**< URL is malformed or uses an unsupported
                                      scheme */
  ccol_http_transfer_aborted = -10, /**< Network send or receive error, or
                                      the stream callback aborted */
  ccol_msg_too_large = -9,       /**< Message data exceeded the configured size
                                   limit */
  ccol_container_empty = -8,     /**< Container has no elements */
  ccol_container_full = -7,      /**< Container at maximum capacity */
  ccol_timed_out = -6,           /**< Operation timed out */
  ccol_not_permitted = -5,       /**< Operation not permitted in this state */
  ccol_invalid_args = -4,        /**< The caller gave invalid arguments */
  ccol_key_not_found = -3,       /**< Key does not exist in map */
  ccol_key_already_present = -2, /**< Key already exists (for update
                                    operations) */
  ccol_not_enough_memory = -1,   /**< Memory allocation failed */
  ccol_success = 0               /**< Operation succeeded */
} ccol_retval_t;

/** Returns a string literal for @p r. You can use it in a ccol_fatal_err()
 * message.
 */
static inline const char *ccol_retval_to_str(ccol_retval_t r) {
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

/**
 * Writes a hex dump of @p size bytes at @p data to stderr. The format is like
 * the one of xxd: an offset, hex columns, and a sidebar of printable ASCII.
 * The cbmap and chmap macros call this automatically before a
 * ccol_fatal_err() for a failed insert or a failed lookup. This is why the
 * key that caused the failure is visible, even when it is opaque binary
 * data.
 */
static inline void _ccol_dump_key_to_stderr(const void *data, size_t size) {
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

/**
 * @brief Attribute for automatic cleanup on scope exit
 *
 * This macro uses the cleanup attribute of GCC and Clang. The attribute
 * calls destructor when the variable goes out of scope.
 *
 * @param destructor Function to call with pointer to variable
 *
 * Example:
 * @code
 * void cleanup_int(int **p) { free(*p); *p = NULL; }
 * int *ptr _ccol_destructor(cleanup_int) = malloc(sizeof(int));
 * // the cleanup of ptr happens automatically at the end of the scope
 * @endcode
 */
#define _ccol_destructor(destructor) __attribute__((cleanup(destructor)))

/* ========================================================================== */
/*                    CUSTOM MEMORY MANAGEMENT TYPES                          */
/* ========================================================================== */

/**
 * @brief Custom malloc function pointer type
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
typedef void *(*ccol_malloc_t)(size_t size);

/**
 * @brief Custom free function pointer type
 * @param ptr Pointer to free
 */
typedef void (*ccol_free_t)(void *ptr);

/**
 * @brief Custom calloc function pointer type
 * @param elem_count Number of elements
 * @param elem_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
typedef void *(*ccol_calloc_t)(size_t elem_count, size_t elem_size);

/**
 * @brief Custom realloc function pointer type
 * @param ptr Existing pointer
 * @param size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
typedef void *(*ccol_realloc_t)(void *ptr, size_t size);

/**
 * @brief Custom memory management procedures
 *
 * A struct that holds the custom functions that allocate and free memory.
 * Give this struct to a collection to create that collection with custom
 * memory management.
 *
 * @note All four function pointers must be non-NULL when you give this struct
 * @note The functions must behave in the same way as the standard malloc,
 * free, calloc and realloc
 * @note That includes alignment. Every block that malloc, calloc and realloc
 * return must be aligned for any object type, which is
 * _Alignof(max_align_t), exactly as the standard functions guarantee. The
 * containers place entries that carry that alignment at the start of a
 * block, and a less aligned block makes every access to such an entry
 * undefined behavior. A pool or arena allocator that hands out blocks at a
 * smaller granularity must round each block up to that alignment.
 */
typedef struct ccol_memmgmt_procs_t {
  ccol_malloc_t malloc;   /**< Custom malloc */
  ccol_free_t free;       /**< Custom free */
  ccol_calloc_t calloc;   /**< Custom calloc */
  ccol_realloc_t realloc; /**< Custom realloc */
} ccol_memmgmt_procs_t;

/* ========================================================================== */
/*                    COMPARISON AND HASHING TYPES                            */
/* ========================================================================== */

/**
 * @brief Custom comparison function type
 *
 * The library uses this for a sort and for an ordered map. The function must
 * return:
 * - Negative if first < second
 * - Zero if first == second
 * - Positive if first > second
 *
 * @param first Pointer to first element
 * @param second Pointer to second element
 * @return Comparison result (negative/zero/positive)
 *
 * Example:
 * @code
 * int compare_ints(const void *a, const void *b) {
 *   int ia = *(const int*)a;
 *   int ib = *(const int*)b;
 *   return (ia > ib) - (ia < ib);
 * }
 * @endcode
 */
typedef int (*ccol_comparison_proc_t)(const void *first, const void *second);

/**
 * @brief Custom hashing function type
 *
 * The library uses this for a hash map. The function must return a hash
 * value for the data that it gets. A good hash function spreads the values
 * uniformly.
 *
 * @param ptr Pointer to data to hash
 * @param size Size in bytes of the data at ptr
 * @return Hash value (unsigned long)
 *
 * @note For a key type of a fixed size, size is the fixed key size of the
 * map. The key type that the caller constructed the map with sets that size.
 * A binary key can have a variable length. Such a key is a string, or
 * another key that is like a buffer. For it, size is the byte length that
 * the caller gives. A hash function
 * can therefore always hash exactly ptr[0..size). It needs no outside
 * convention, such as a length prefix or a NUL terminator, to find where it
 * must stop.
 *
 * Example:
 * @code
 * unsigned long hash_bytes(const void *p, size_t size) {
 *   const unsigned char *b = (const unsigned char *)p;
 *   unsigned long h = 2166136261UL;
 *   for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 16777619UL;
 *   return h;
 * }
 * @endcode
 */
typedef unsigned long (*ccol_hashing_proc_t)(const void *ptr, size_t size);

/**
 * @brief Custom key equality function type
 *
 * A hash map uses this, beside a ccol_hashing_proc_t, to decide whether two
 * keys are the same key. The function returns true when the two keys are
 * equal.
 *
 * @param first Pointer to the first key
 * @param first_size Size in bytes of the key at first
 * @param second Pointer to the second key
 * @param second_size Size in bytes of the key at second
 * @return true when the two keys are the same key, and false otherwise
 *
 * @note The function must agree with the hash function that the map uses:
 * two keys that it reports as equal must get the same hash. A pair of keys
 * that it reports as equal with different hashes can land in different
 * buckets, and a lookup then misses a key that is present.
 * @note first and second carry no alignment for the key type. Read a field
 * through memcpy, or through a pointer to a type whose alignment is 1.
 *
 * Example:
 * @code
 * typedef struct { char tag; long id; } tagged_id;  // padding after tag
 * bool tagged_id_eq(const void *a, size_t as, const void *b, size_t bs) {
 *   tagged_id x, y;
 *   (void)as; (void)bs;
 *   memcpy(&x, a, sizeof(x));
 *   memcpy(&y, b, sizeof(y));
 *   return x.tag == y.tag && x.id == y.id;
 * }
 * @endcode
 */
typedef bool (*ccol_key_equality_proc_t)(const void *first, size_t first_size,
                                         const void *second,
                                         size_t second_size);

/* ========================================================================== */
/*                         MAP KEY-VALUE TYPES                                */
/* ========================================================================== */

/**
 * @brief Key or value pair for map types
 *
 * A generic struct that holds a pointer and a size. Every map (chmap, cbmap)
 * uses it to hold a key and a value of any type and of any size.
 *
 * @note ptr points to the real data, which the map copies into itself
 * @note for a string, size counts the null terminator
 */
typedef struct cmap_pair {
  void *ptr;   /**< Pointer to data */
  size_t size; /**< Size of data in bytes */
} cmap_pair;

/**
 * @brief Iterator for map types
 *
 * A generic iterator struct that every map uses. It gives access to the
 * current key-value pair of an iteration.
 *
 * @note The pointers stay valid until somebody changes the map
 * @note You must destroy the iterator at the end, or let it go out of scope
 */
typedef struct cmap_iterator {
  /* Both pairs point at the accessor of the container for the current entry.
   * For cbstmap that accessor is the authoritative pair of the node. For
   * chashmap it is what every later lookup of that key reports. An iterator
   * exists so that you can read them, and so that you can write the bytes
   * that ptr addresses inside size. An assignment to ptr itself, or to size
   * itself, is a compile error, because the two describe one another. Without
   * the qualifier, a caller that goes around ccol_iter_key_ptr() and
   * ccol_iter_val_ptr() could store a pointer that the container never
   * allocated. The teardown of cbstmap then frees that pointer. */
  const cmap_pair *key_pair; /**< Pointer to current key */
  const cmap_pair *val_pair; /**< Pointer to current value */
  struct cmap_iterator *(*_next_fn)(struct cmap_iterator *); /**< Advance fn */
  void (*_free_fn)(struct cmap_iterator *);                  /**< Destroy fn */
  bool _direct_ptr; /**< true = val_pair->ptr IS the element (vec); false =
                       the SSO rules of the map apply */
} cmap_iterator;

/* ========================================================================== */
/*                    MEMORY MANAGEMENT UTILITIES                             */
/* ========================================================================== */

/**
 * @brief Verify custom memory management procedures are valid
 *
 * Checks that every necessary function pointer is non-NULL when the caller
 * gives a custom memory management struct.
 *
 * @param mmgt_procs The memory management procs to verify (or NULL)
 * @param err An optional pointer that receives an error string
 * @return true if the procs are valid or NULL, false if they are invalid
 *
 * @note If mmgt_procs is NULL, the result is true, and the library uses the
 * default malloc and free
 * @note The caller must give all four functions when the struct is
 * non-NULL
 */
#define ccol_verify_memmgmt_procs(mmgt_procs, err)                         \
  ({                                                                       \
    bool __ccol_vmp_ok = true;                                             \
    if ((mmgt_procs) && (!(mmgt_procs)->malloc || !(mmgt_procs)->calloc || \
                         !(mmgt_procs)->realloc || !(mmgt_procs)->free)) { \
      if ((err)) {                                                         \
        *(err) = CCOL_ERR_STR(                                             \
            "Detected at least one NULL memory management function");      \
      }                                                                    \
      __ccol_vmp_ok = false;                                               \
    }                                                                      \
    __ccol_vmp_ok;                                                         \
  })

/**
 * @brief Populate container's memory management procedures
 *
 * Allocates memory for the custom memory management procs and copies them
 * into the container struct. The library uses this when it creates a
 * container.
 *
 * @param container The container struct to populate
 * @param mmgmt_procs The source memory management procs (or NULL)
 * @param err An optional pointer that receives an error string
 * @return true for a success, false if the allocation fails
 *
 * @note If mmgmt_procs is NULL, this sets container->m_procs to NULL, and the
 * container uses the default procs
 * @note The allocator that the caller gives allocates the memory for
 * m_procs
 */
#define ccol_populate_mem_mgmt_procs(container, mmgmt_procs, err)  \
  ({                                                               \
    bool __ccol_pmp_ok = true;                                     \
    if ((mmgmt_procs)) {                                           \
      (container)->m_procs =                                       \
          (mmgmt_procs)->malloc(sizeof(ccol_memmgmt_procs_t));     \
      if (!(container)->m_procs) {                                 \
        if ((err)) {                                               \
          *(err) = CCOL_ERR_STR(                                   \
              "Failed to allocate buffer for memory mgmt buffer"); \
        }                                                          \
        __ccol_pmp_ok = false;                                     \
      } else {                                                     \
        memcpy((container)->m_procs, (mmgmt_procs),                \
               sizeof(ccol_memmgmt_procs_t));                      \
      }                                                            \
    } else {                                                       \
      (container)->m_procs = NULL;                                 \
    }                                                              \
    __ccol_pmp_ok;                                                 \
  })

/**
 * @brief Bookkeeping for ccol_scoped_ptr / ccol_scoped_ptr_mp
 *
 * Do not construct this struct directly. The scoped_ptr macros populate
 * every field themselves. It exists only so that the cleanup callback can
 * find two things at the end of the scope. The first is the allocator that
 * must free the pointer. The second is the current value of the pointer.
 * A later call, for example to
 * _ccol_mem_alloc, can give the pointer a new value after its declaration.
 */
typedef struct ccol_scoped_ptr_ctx_t {
  void **ptr_addr;               /**< Address of the guarded pointer var */
  ccol_memmgmt_procs_t *m_procs; /**< Allocator to free it with, or NULL */
} ccol_scoped_ptr_ctx_t;

/**
 * @brief Cleanup callback bound by ccol_scoped_ptr / ccol_scoped_ptr_mp
 * @param ctx Address of the hidden companion variable the macros declare
 */
static inline void _ccol_scoped_ptr_cleanup(ccol_scoped_ptr_ctx_t *ctx) {
  if (ctx->ptr_addr && *ctx->ptr_addr) {
    _ccol_mem_free(ctx->m_procs, *ctx->ptr_addr);
    *ctx->ptr_addr = NULL;
  }
}

/**
 * @brief Declare a raw pointer that is freed automatically at scope exit
 *
 * The macro declares `type *name` and sets it to NULL. Assign a value to it
 * in the usual way. For example, use mmgmt_procs->malloc(size), or a plain
 * malloc() when mmgmt_procs is NULL. At the end of the enclosing scope, the
 * macro frees the value that name holds. It frees that value with
 * mmgmt_procs->free, or with free() when mmgmt_procs is NULL.
 *
 * @param name The name of the pointer variable to declare
 * @param type The type that the pointer points to (for example char,
 *             struct foo)
 * @param mmgmt_procs The custom allocator that frees the last value of name,
 *                     or NULL for the default allocator
 *
 * @note The macro stores mmgmt_procs by reference, and does not copy it. It
 *       must stay valid for at least as long as the enclosing scope of name.
 *       This is true naturally when mmgmt_procs is a variable that is already
 *       in scope at the point of the declaration
 * @note The macro frees only the last value of name. You must free an earlier
 *       value yourself before you give name a new value inside the scope.
 *       Every other guard that uses the cleanup attribute works in the same
 *       way
 * @note The macro expands to two declarations, and not to one expression. Use
 *       it as its own statement. Every *_construct_scoped macro in this
 *       library already has the same constraint
 * @note Use ccol_scoped_ptr_release() to give the ownership to code outside
 *       the enclosing scope. The macro then does not free name
 *       automatically
 *
 * Example:
 * @code
 * void process(ccol_memmgmt_procs_t *mp) {
 *   ccol_scoped_ptr_mp(buf, char, mp);
 *   buf = mp ? mp->malloc(128) : malloc(128);
 *   if (!buf) return;
 *   // mp frees buf on every return path below this point
 * }
 * @endcode
 */
#define ccol_scoped_ptr_mp(name, type, mmgmt_procs)               \
  type *name = NULL;                                              \
  ccol_scoped_ptr_ctx_t name##__ccol_scoped_ctx _ccol_destructor( \
      _ccol_scoped_ptr_cleanup) = {(void **)&(name), (mmgmt_procs)}

/**
 * @brief Declare a raw pointer that the default allocator frees
 *        automatically at scope exit
 *
 * This is the same as ccol_scoped_ptr_mp(name, type, NULL). See that macro
 * for the full contract.
 */
#define ccol_scoped_ptr(name, type) ccol_scoped_ptr_mp(name, type, NULL)

/**
 * @brief Release the ownership of a scoped pointer, so that nothing frees
 *        it automatically
 *
 * Returns the current value of name and sets name to NULL. The cleanup that
 * ccol_scoped_ptr or ccol_scoped_ptr_mp registered then becomes a no-op at
 * the end of the scope. Use this macro to give the ownership of the pointer
 * to code outside the enclosing scope, for example as the return value of a
 * function. The pointer is then not freed in that scope.
 *
 * @param name A pointer that ccol_scoped_ptr or ccol_scoped_ptr_mp declared
 *             earlier
 * @return The value of name from before the release
 *
 * Example:
 * @code
 * char *build(ccol_memmgmt_procs_t *mp) {
 *   ccol_scoped_ptr_mp(buf, char, mp);
 *   buf = mp ? mp->malloc(128) : malloc(128);
 *   if (!buf) return NULL;
 *   // ... populate buf ...
 *   return ccol_scoped_ptr_release(buf); // caller now owns it
 * }
 * @endcode
 */
#define ccol_scoped_ptr_release(name)              \
  ({                                               \
    __typeof__(name) __ccol_released_ptr = (name); \
    (name) = NULL;                                 \
    __ccol_released_ptr;                           \
  })

/* ========================================================================== */
/*                         TYPE INTROSPECTION                                 */
/* ========================================================================== */

/**
 * @brief Check if type is an integral or floating-point type
 *
 * This macro uses C11 _Generic to find out whether a value has a standard
 * numeric type. Such a type is a signed integer, an unsigned integer or a
 * floating-point type, with const or without const.
 *
 * @param x The value to check
 * @return true for a numeric type, and false for every other type
 *
 * @note It handles char, short, int, long and long long, signed and unsigned
 * @note It handles float, double and long double
 * @note It handles the const form of every type above
 */
#if defined __clang__
#define ccol_is_integral_type(x)                                            \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool __ccol_iit_result = _Generic((x),                                  \
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
    __ccol_iit_result;                                                      \
  })
#else
#define ccol_is_integral_type(x)      \
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

/**
 * @brief Check if type is a pointer to integral or floating-point type
 *
 * This macro uses C11 _Generic to find out whether a pointer points to a
 * standard numeric type.
 *
 * @param x The pointer to check
 * @return true for a pointer to a numeric type, and false for every other
 *         pointer
 *
 * @note It handles a pointer to every type that ccol_is_integral_type()
 * checks
 */
#if defined __clang__
#define ccol_is_integral_ptr(x)                                             \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool __ccol_iip_result = _Generic((x),                                  \
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
    __ccol_iip_result;                                                      \
    _Pragma("GCC diagnostic pop");                                          \
  })
#else
#define ccol_is_integral_ptr(x)         \
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

/**
 * @brief Check if pointer points to signed integer type
 *
 * This macro uses C11 _Generic to find out whether a pointer points to a
 * signed integer type. The BST map uses it to choose how it compares a key.
 *
 * @param _ptr The pointer to check
 * @return true for a pointer to a signed integer, and false for every other
 *         pointer
 *
 * @note It returns true for char*, short*, int*, long* and long long*
 * @note It returns true for the const form of each of them
 * @note It returns false for an unsigned type and for a type that is not an
 * integer
 */
#define __is_signed_int_ptr(_ptr) \
  _Generic((_ptr),                \
      char *: true,               \
      signed char *: true,        \
      short *: true,              \
      int *: true,                \
      long *: true,               \
      long long *: true,          \
      const char *: true,         \
      const signed char *: true,  \
      const short *: true,        \
      const int *: true,          \
      const long *: true,         \
      const long long *: true,    \
      default: false)

/**
 * @brief Check if data is a char pointer (string)
 *
 * This macro uses C11 _Generic to find out whether data is a pointer to a
 * char type. It handles a signed char pointer and an unsigned char pointer.
 *
 * @param data The value to check
 * @return true for a char pointer, and false for every other value
 *
 * @note The Clang form suppresses the warnings about unreachable code
 * @note It treats unsigned char* as a string type
 * @note It returns false for a char array. Use ccol_is_char_array() for an
 * array
 */
/* Gives 1 when the TYPE of data is an array of a character type, and 0 when
 * it is a character pointer. Only a character array or a character pointer
 * reaches this through ccol_is_char_array(), because ccol_is_char_ptr()
 * guards it.
 *
 * _ccol_char_ptr_flavour() names the character-pointer type that data
 * decays to. A pointer has that type already, so the two are compatible. An
 * array has an array type, which is never compatible with a pointer type.
 * __builtin_types_compatible_p() ignores the top-level qualifiers of both
 * operands. A pointer object that is itself const or volatile, such as
 * `const char *const NAME`, is therefore still a pointer here. A test on the
 * type of &(data) cannot give that answer, because &(data) carries the
 * top-level qualifier of data into the pointed-to type, and a _Generic list
 * would have to name every qualified spelling of every character pointer.
 *
 * The expression is an integer constant expression, it never evaluates
 * data, and it needs no address of data, so an rvalue is valid here. */
#define _ccol_type_is_array_of_char(data)          \
  (!__builtin_types_compatible_p(__typeof__(data), \
                                 __typeof__(_ccol_char_ptr_flavour(data))))

#if defined __clang__
#define ccol_is_char_ptr(data)                                              \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool __ccol_icp_result = _Generic((data),                               \
        char *: true,                                                       \
        const char *: true,                                                 \
        signed char *: true,                                                \
        const signed char *: true,                                          \
        unsigned char *: true,                                              \
        const unsigned char *: true,                                        \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    __ccol_icp_result;                                                      \
  })

/**
 * @brief Check if data is a char array (not pointer to pointer)
 *
 * This macro separates a char array (char arr[]) from a char pointer
 * (char*).
 *
 * @param data The value to check. The macro reads only its type, so an
 * rvalue is as valid as an lvalue.
 * @return true for a char array, and false for a pointer or another type
 *
 * @note A char pointer that is itself qualified, such as a
 * `static const char *const NAME` or a `char *volatile p`, is a pointer and
 * not an array. The answer depends only on whether the type of data is an
 * array type.
 * @note It helps you choose how to store a string
 */
#define ccol_is_char_array(data) \
  (ccol_is_char_ptr((data)) && _ccol_type_is_array_of_char(data))
#else
#define ccol_is_char_ptr(data)     \
  _Generic((data),                 \
      char *: true,                \
      const char *: true,          \
      signed char *: true,         \
      const signed char *: true,   \
      unsigned char *: true,       \
      const unsigned char *: true, \
      default: false)

#define ccol_is_char_array(data) \
  (ccol_is_char_ptr((data)) && _ccol_type_is_array_of_char(data))
#endif

/* Every enumerator below has an explicit value, and this is deliberate. C
 * gives the same values automatically when they are implicit. A list with
 * implicit values renumbers every later entry quietly. This happens as soon
 * as somebody inserts a new enumerator anywhere except at the very end. This
 * is the same hazard that ccol_retval_t guards against (see the doc comment
 * of that enum in this file). Add a new enumerator at the very end, with the
 * next free number. That is what keeps the value of every enumerator that
 * already exists. */
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
  ccol_signed_char = 16, /**< A scalar `signed char` (or `int8_t`) key type
                           or value type. It is not the same as ccol_char.
                           The platform defines the signedness of a plain
                           `char`. This is why `signed char` needs its own
                           type, so that the comparison is truly signed on
                           every platform (see compare_keys() in cbstmap.c).
                           A `signed char *` still resolves to ccol_string,
                           and this does not change it. */
} ccol_data_type;

/**
 * @brief The byte size of the C type that a ccol_data_type names, or 0 when
 * the type has no single fixed width
 *
 * Returns sizeof() of the C type that the enumerator stands for. It does this
 * for every enumerator that names one specific C type. ccol_string,
 * ccol_other_types and any value that the function does not recognize return
 * 0. A key or a value of those types can correctly have any size. There is
 * therefore no one width to check a cmap_pair of a caller against.
 *
 * A caller can build a cmap_pair by hand and pass it through the raw layer
 * of a map module, such as chmap_insert_elem() or cbmap_insert_elem(). The
 * map modules use this function to validate such a pair. A container that
 * dispatches on
 * its declared key type reads a fixed number of bytes out of the key that it
 * gets. This is why the container rejects a key_pair whose size disagrees
 * with this value, with ccol_invalid_args. Without that check, the container
 * reads past the buffer of the caller. It can also store a key that no
 * lookup with a correct type can find again. A caller that builds a
 * cmap_pair by hand can
 * use this function to give the pair the correct size.
 *
 * @param type The data type enumerator to size
 *
 * @return sizeof() of the C type that matches the enumerator, or 0 if the
 * type has no fixed width
 */
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

#if defined __clang__
#define _determine_non_special_data_type(var)                               \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    ccol_data_type __ccol_dnsdt_result = _Generic((var),                    \
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
    __ccol_dnsdt_result;                                                    \
  })
#else
#define _determine_non_special_data_type(var)            \
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

/* __ccol_dcdt_type_probe is a new, zero-initialized object that has the type
 * of (data). The code below uses it in place of (data) itself. The operand of
 * __builtin_classify_type() is a pure compile-time classification of a type.
 * It works like the operand of sizeof or of _Generic. Nothing ever
 * evaluates it for its VALUE. But the -Wuninitialized analysis of Clang does
 * not treat it
 * as an unevaluated context, and it does treat sizeof and _Generic that way.
 *
 * A caller can write a (data) that dereferences a companion variable that
 * tracks a type and that stays uninitialized on purpose. One example is
 * hm_name##__ccol_key_type_var in chmap_declare(). That variable must stay
 * uninitialized, so that the same macro is also valid as the declaration of
 * a struct member. C does not permit an initializer there at all. With such a
 * (data), a direct use starts a false "used before initialized" diagnostic.
 *
 * __ccol_dcdt_type_probe has exactly the type of (data). __typeof__(data) is
 * itself unevaluated, so this does not depend on the value of (data) either.
 * The probe is a well-defined object, so nothing here reads indeterminate
 * memory. This closes the false positive. It changes neither the behavior of
 * this macro nor the single-evaluation contract of (data). The name is
 * specific enough that the (data) expression of a real caller cannot collide
 * with it. ccol_scoped_ptr_release and __ccol_released_ptr in this header
 * follow the same precedent, for the same reason. */
#define ccol_determine_ccol_data_type(data)                           \
  ({                                                                  \
    ccol_data_type __ccol_dcdt_r = ccol_other_types;                  \
    if (ccol_is_char_array((data))) {                                 \
      __ccol_dcdt_r = ccol_string;                                    \
    } else if (ccol_is_char_ptr((data))) {                            \
      __ccol_dcdt_r = ccol_string;                                    \
    } else {                                                          \
      __typeof__(data) __ccol_dcdt_type_probe = {0};                  \
      if (__builtin_classify_type(__ccol_dcdt_type_probe) ==          \
              5 && /* is a pointer */                                 \
          sizeof(__ccol_dcdt_type_probe) ==                           \
              sizeof(uintptr_t)) { /* other pointers */               \
        __ccol_dcdt_r = ccol_pointer;                                 \
      } else {                                                        \
        __ccol_dcdt_r =                                               \
            _determine_non_special_data_type(__ccol_dcdt_type_probe); \
      }                                                               \
    }                                                                 \
    __ccol_dcdt_r;                                                    \
  })

/**
 * @brief Compile-time predicate: does this lvalue have a pointer-to-character
 *        type?
 *
 * The answer is an integer constant expression, so __builtin_choose_expr()
 * below can select on it. ccol_is_char_ptr() cannot serve here. Its Clang form
 * is a statement expression, which is never a constant expression.
 *
 * The list is the same one that ccol_is_char_ptr() carries, and the two must
 * stay in step. Both describe the set of types that this library treats as a
 * string, which is a run of bytes with its own length, and not as a scalar.
 *
 * @param lvalue An expression whose TYPE the macro inspects. The macro never
 *               evaluates it.
 */
#define _ccol_type_is_char_ptr(lvalue)                                      \
  (__builtin_types_compatible_p(__typeof__(lvalue), char *) ||              \
   __builtin_types_compatible_p(__typeof__(lvalue), const char *) ||        \
   __builtin_types_compatible_p(__typeof__(lvalue), signed char *) ||       \
   __builtin_types_compatible_p(__typeof__(lvalue), const signed char *) || \
   __builtin_types_compatible_p(__typeof__(lvalue), unsigned char *) ||     \
   __builtin_types_compatible_p(__typeof__(lvalue), const unsigned char *))

/**
 * @brief A null pointer whose TYPE is the character-pointer flavour of expr,
 *        with an array already decayed to a pointer.
 *
 * _ccol_declared_or_own_type() reads only the type of this expression. Every
 * association is a cast of a null pointer constant that this header writes
 * itself, so each one is well formed for any expr. That matters, because
 * __builtin_choose_expr() type-checks the arm that it does not choose.
 *
 * The default association answers char * for an expr that is not a character
 * pointer at all. Nothing reaches it through a correct call: a container
 * whose declared type is a character pointer that a caller keys with, say, a
 * struct, meets the type error at the initializer that follows, which is
 * where a reader can act on it.
 *
 * @param expr An expression whose TYPE the macro inspects. _Generic never
 *             evaluates its controlling expression.
 */
#define _ccol_char_ptr_flavour(expr)                   \
  _Generic((expr),                                     \
      char *: (char *)0,                               \
      const char *: (const char *)0,                   \
      signed char *: (signed char *)0,                 \
      const signed char *: (const signed char *)0,     \
      unsigned char *: (unsigned char *)0,             \
      const unsigned char *: (const unsigned char *)0, \
      default: (char *)0)

/**
 * @brief The type that a container macro gives the private copy it makes of a
 *        caller expression.
 *
 * type_var is the companion type variable that a *_declare or *_construct
 * macro made for the container, so *(type_var) is an lvalue of the DECLARED
 * key type or value type. expr is the expression that the caller wrote.
 *
 * For a declared type that is NOT a character pointer, the macro gives the
 * declared type. The copy is then a plain C assignment, and it converts the
 * value of expr through the conversion rules of the compiler. This is what
 * stops a same-width expression of another type from reaching the container
 * as a raw bit pattern. An expression with no implicit conversion to the
 * declared type is a compile error at the point of the copy.
 *
 * For a declared type that IS a character pointer, the macro gives the type of
 * expr itself. Every character-pointer type is one single key type and one
 * single value type to this library: char *, signed char * and unsigned char *
 * all classify as ccol_string, a NUL-terminated string, and the container
 * compares the bytes of that string and never the pointer type. A map keyed
 * on unsigned char * must therefore stay reachable through a char * that
 * addresses the same string. A conversion to the declared type would make
 * that a -Wpointer-sign error and would take the feature away. No
 * bit pattern can be reinterpreted here either way, because every one of these
 * types reaches the same string branch of _populate_cmap_pair().
 *
 * _ccol_char_ptr_flavour() supplies the type of the character-pointer arm.
 * It is not __typeof__(expr). A caller that writes a char buf[64] key, or a
 * string literal, would otherwise give that arm an ARRAY type, and an array
 * is not assignable from another array. The decayed pointer is also what
 * _populate_cmap_pair() wants: its ccol_is_char_ptr() branch reads the
 * pointer and measures the string with strlen + 1, which is exactly the pair
 * that its array branch produces.
 *
 * __builtin_choose_expr() type-checks BOTH arms, and only the chosen arm
 * decides the type. Every arm must therefore be a well-formed expression for
 * every type that any caller can reach here. A plain "(expr) + 0" is not: it
 * fails for a struct value type, and it fails for a pointer to an incomplete
 * type such as the cvec value of a map of vectors. _Generic answers the same
 * question with associations that this header writes itself, so each one is
 * well formed whatever expr turns out to be.
 *
 * The comma operator in front of the choice applies an lvalue conversion,
 * so the type carries no qualifier. A map declared with a const key type
 * therefore still gets a modifiable copy, which _ccol_clear_padding() can
 * write to.
 *
 * The macro never evaluates expr. __typeof__() does not evaluate its operand,
 * and __builtin_choose_expr() evaluates neither arm. A caller expression with a
 * side effect is therefore still evaluated exactly one time, by the
 * initializer that follows this type.
 *
 * @param type_var The companion type variable of the container
 * @param expr     The expression that the caller wrote
 */
#define _ccol_declared_or_own_type(type_var, expr)                      \
  __typeof__((void)0,                                                   \
             __builtin_choose_expr(_ccol_type_is_char_ptr(*(type_var)), \
                                   _ccol_char_ptr_flavour(expr), *(type_var)))

/**
 * @brief Set every padding byte of *obj_ptr to zero, where the compiler can.
 *
 * A container that hashes or compares a key of a type that it does not know,
 * such as a struct, reads every byte of the key, and that includes the
 * padding. C leaves the padding of a struct unspecified after a store of the
 * struct value, and an optimizing compiler does copy such a value member by
 * member, so a copy of a key whose padding the caller zeroed can still carry
 * stack garbage in its padding. The container macros call this on their own
 * copy of a key, so that two keys with equal members give equal bytes.
 *
 * __builtin_clear_padding() does exactly that where the compiler offers it.
 * Elsewhere the macro does nothing, and the copy keeps whatever bytes the
 * compiler copied from the key of the caller. For a type with no padding the
 * call is empty either way.
 *
 * @param obj_ptr A pointer to a modifiable object of a complete type
 */
#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
#define _ccol_clear_padding(obj_ptr) __builtin_clear_padding(obj_ptr)
#endif
#endif
#ifndef _ccol_clear_padding
#define _ccol_clear_padding(obj_ptr) ((void)0)
#endif

/* ========================================================================== */
/*                    MAP PAIR POPULATION UTILITY                             */
/* ========================================================================== */

/**
 * @brief Populate a cmap_pair from data of any type
 *
 * This macro finds the correct way to populate a cmap_pair from the type of
 * the data. It handles a string (a char array and a char pointer) in a
 * special way, so that the size counts the null terminator.
 *
 * @param pair A pointer to the cmap_pair to populate
 * @param data The data to store (a value, an array, or a pointer)
 *
 * @note For a char array it stores a pointer to the array, and the size
 * counts the null terminator
 * @note For a char pointer it dereferences to get the string, and the size
 * counts the null terminator. A NULL char pointer gives ptr == NULL and a
 * size of 0, which every raw entry point refuses as ccol_invalid_args
 * @note For every other type it stores a pointer to the data, and the size is
 * sizeof(data)
 * @note It suppresses the array-bounds warning of GCC for the dereference of
 * a char pointer
 *
 * Example:
 * @code
 * cmap_pair kp, vp;
 * int key = 42;
 * char value[] = "hello";
 * _populate_cmap_pair(&kp, key);     // kp.ptr = &key, kp.size = 4
 * _populate_cmap_pair(&vp, value);   // vp.ptr = value, vp.size = 6
 * @endcode
 */
#define _populate_cmap_pair(pair, data)                                \
  do {                                                                 \
    if (ccol_is_char_array((data))) {                                  \
      (pair)->ptr = (char *)&(data);                                   \
      (pair)->size = strlen((char *)(pair)->ptr) + 1;                  \
    } else if (ccol_is_char_ptr((data))) {                             \
      char *__ccol_pcp_addr = (char *)&(data);                         \
      _Pragma("GCC diagnostic push");                                  \
      _Pragma("GCC diagnostic ignored \"-Warray-bounds\"");            \
      (pair)->ptr = *((char **)__ccol_pcp_addr);                       \
      _Pragma("GCC diagnostic pop");                                   \
      /* A NULL pointer is not a string. It gives a pair of size 0,    \
       * which the raw layer refuses as ccol_invalid_args, and strlen  \
       * never reads through it. */                                    \
      (pair)->size =                                                   \
          (pair)->ptr ? strlen((char *)(pair)->ptr) + 1 : (size_t)0;   \
    } else {                                                           \
      /* cmap_pair.ptr is void *. Every entry point that reads a pair  \
       * takes it as const cmap_pair * and only reads the bytes. The   \
       * cast is what lets a caller key a map from a const array.      \
       * Without it, -Wdiscarded-qualifiers makes that a hard error in \
       * a -Werror build. This layer never writes through that const.  \
       */                                                              \
      (pair)->ptr = (void *)&(data);                                   \
      (pair)->size = sizeof((data));                                   \
    }                                                                  \
  } while (0)

/* ========================================================================== */
/*                         UTILITY MACROS                                     */
/* ========================================================================== */

/**
 * @brief Return minimum of two values
 *
 * This macro evaluates both arguments and returns the smaller value.
 *
 * @param a The first value
 * @param b The second value
 * @return The smaller of a and b
 *
 * @warning The macro can evaluate an argument more than one time
 */
#define ccol_min(a, b) ((a) < (b) ? (a) : (b))

/**
 * @brief Return maximum of two values
 *
 * This macro evaluates both arguments and returns the larger value.
 *
 * @param a The first value
 * @param b The second value
 * @return The larger of a and b
 *
 * @warning The macro can evaluate an argument more than one time
 */
#define ccol_max(a, b) ((a) > (b) ? (a) : (b))

/**
 * @brief Type-inferred comparison macro
 *
 * This macro compares the two values of type T that ptr1 and ptr2 address. It
 * returns the standard result of a comparison.
 *
 * @param ptr1 A pointer to the first value
 * @param ptr2 A pointer to the second value
 * @param T The type of the two values
 * @return A negative value if *ptr1 < *ptr2, 0 if they are equal, and a
 *         positive value if *ptr1 > *ptr2
 *
 * @note It uses the three-way comparison (a > b) - (a < b)
 * @note For an integer type the result is -1, 0 or 1
 * @note ptr1 and ptr2 need no alignment for T, and the object that each one
 *       addresses may have any effective type. See the note below.
 */
/* The macro reads both operands with memcpy. It does not dereference a
 * pointer that it cast to T *. Two separate requirements make the cast wrong,
 * and satisfying one of them says nothing about the other.
 *
 * The first is alignment. A caller reaches this macro through the raw
 * cmap_pair layer of cbstmap, which is cbmap_insert_elem, cbmap_get_elem_ref
 * and cbmap_delete_elem. That layer accepts a pair that the caller built by
 * hand, and such a pair carries no alignment guarantee at all. Key bytes that
 * a caller parsed out of a packed network frame sit at whatever offset the
 * frame put them. A load through a T * from that address is undefined. It
 * faults on a strict-alignment target, and UndefinedBehaviorSanitizer reports
 * it on every target ("load of misaligned address ... which requires N byte
 * alignment").
 *
 * The second is the effective type. C11 6.5p7 permits a load through an
 * lvalue of type T only when the stored object really has type T. The bytes
 * behind a cmap_pair are type-erased by construction, so the cast violates
 * that rule at ANY address, aligned or not. Nothing traps on it; the
 * optimizer simply takes a licence it was never entitled to.
 *
 * memcpy with a size that is a compile-time constant answers both. The
 * compiler still emits the single load that the cast emitted, so this costs
 * nothing. chashmap already reads every key this way, and the comment on
 * hash_key_data in chashmap.c gives the same two reasons. */
#define ccol_typed_cmp(ptr1, ptr2, T)                                          \
  ({                                                                           \
    T __ccol_tcmp_lhs;                                                         \
    T __ccol_tcmp_rhs;                                                         \
    memcpy(&__ccol_tcmp_lhs, (ptr1), sizeof(T));                               \
    memcpy(&__ccol_tcmp_rhs, (ptr2), sizeof(T));                               \
    (__ccol_tcmp_lhs > __ccol_tcmp_rhs) - (__ccol_tcmp_lhs < __ccol_tcmp_rhs); \
  })

#ifdef RUNNING_UNIT_TESTS
/*
 * The order of the fork-prepare handlers, checked and not assumed.
 *
 * Every module here that registers a pthread_atfork() prepare handler takes
 * its own locks in that handler. It keeps them until after the fork. The
 * locks of the handlers therefore nest in the order in which the handlers
 * run. That order is the reverse of the registration order. The module that
 * the embedding program uses first decides the registration order. This is
 * why each module forces its dependencies to register ahead of it.
 *
 * Two handlers must never run in opposite relative orders in two different
 * forks. If they do, their locks nest both ways, and the cycle is real and
 * not an artifact of a detector.
 *
 * Each handler reports itself here when it starts. This code records the
 * order of each pair and refuses a later contradiction. The test suites
 * therefore check that property on every fork that they make, and a comment
 * does not merely claim it. The build compiles this only into the test
 * binaries. The shipped library contains none of it.
 */
typedef enum {
  ccol_atfork_module_clogger = 0,
  ccol_atfork_module_cthreadcomm,
  ccol_atfork_module_cthreadpool,
  ccol_atfork_module_chttpserver,
  ccol_atfork_module_chttpclient,
  ccol_atfork_module_count
} ccol_atfork_module_t;

void _ccol_atfork_order_record(ccol_atfork_module_t ccol_module);

/* Starts a new sequence: the next record is the first handler of a fork.
 * The parent and the child handlers that the recorder registers call it
 * after every fork. A test that records sequences with no fork between them
 * calls it between two sequences. */
void _ccol_atfork_order_reset(void);
#endif /* RUNNING_UNIT_TESTS */

#pragma GCC visibility pop
