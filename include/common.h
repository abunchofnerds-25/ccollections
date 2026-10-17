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
 * The base infrastructure that every collection type of the library builds
 * on:
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
 * Every collection in the library includes this header, so thread safety,
 * memory management and error handling look the same everywhere.
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

/* Every declaration from here to the end of this header belongs to the
 * public ABI of libccollections, and the shared library exports all of
 * them. The library is built with -fvisibility=hidden, so a function or an
 * object outside such a block stays internal: it isn't in the dynamic symbol
 * table, and no symbol of the same name in an application linking against
 * the library can interpose it or collide with it. */
#pragma GCC visibility push(default)

/* ========================================================================== */
/*                         FORK SAFETY OPT-OUT                                */
/* ========================================================================== */

/**
 * @brief A compile-time switch that decides whether the build compiles the
 *        fork() safety machinery of this library, which uses
 *        pthread_atfork().
 *
 * Some modules (cthreadpool, the ccol_event_loop and the queue types of
 * cthreadcomm, clogger, chttpserver and chttpclient) register
 * pthread_atfork() prepare, parent and child handlers. A fork() can happen
 * while a thread holds one of their internal locks, and the handlers make
 * sure the child doesn't end up with a mutex that stays locked forever,
 * since the child has no thread left that could unlock it.
 *
 * That protection costs real work on every fork() in the process, from any
 * thread and for any reason. The prepare handler walks its module's
 * process-wide registry and locks the internal lock of every live
 * handle before fork() can go on, and then the parent and child handlers
 * unlock all of them again.
 *
 * Some applications never call fork(), and others always call exec() right
 * after fork() to run a subprocess with fork() and execve(), or through an
 * equivalent posix_spawn() wrapper; the child of such a fork never touches a
 * handle of this library before it replaces its own process image. Those
 * applications gain nothing from the protection, so they can define this
 * switch to 0, for example with -DCCOL_FORK_SAFETY_REQUIRED=0.
 *
 * The build then drops every one of these registrations from the library,
 * along with the handler bodies and the per-handle state that exists only
 * for them (for example a flag saying "a fork() gave this handle to a
 * child"), and the cost per fork() goes away with them.
 *
 * Nothing else about a module's thread safety changes: its own locks stay,
 * and so does the safety of a concurrent create and destroy through the
 * generation-tagged handle tables. The close-on-exec gate below stays too,
 * because it keeps the library's descriptors out of a child that calls
 * exec(), which is exactly the pattern this switch is for. Only the
 * protection that is specific to fork() is controlled by it.
 *
 * The value is 1, which compiles the fork safety in, unless the macro is
 * defined before the first include of this header.
 */
#ifndef CCOL_FORK_SAFETY_REQUIRED
#define CCOL_FORK_SAFETY_REQUIRED 1
#endif

/* OpenBSD's fork() takes the lock of its fork handler list while the
 * process is threaded, but in the child it marks the process as not
 * threaded before the release, so the release does nothing and the lock
 * stays held in the child. Once that child has started a thread, a
 * pthread_atfork() in it waits for ever. Every module of the library
 * registers its fork handlers on first use, which could be in such a child,
 * so on OpenBSD every module registers them when the library loads instead
 * and a child never registers one. The macro is internal. */
#if defined(__OpenBSD__)
#define _CCOL_ATFORK_AT_LOAD 1
#endif

/* ========================================================================== */
/*                         THREADING PRIMITIVES                               */
/* ========================================================================== */

/* Every wrapper in this section is internal, as its leading underscore
 * says. The library reaches pthreads and POSIX semaphores only through
 * these wrappers, so a port to a system without pthreads only needs to
 * replace this header. An application protects its own data with pthreads
 * or <threads.h> directly. */

/* macOS has no pthread_mutex_timedlock(), no pthread_rwlock_timedwrlock(), no
 * pthread_condattr_setclock() and no working sem_init(), so the wrappers
 * below give it each of them. A timed lock polls a try-lock until its
 * deadline, a condition variable records its clock and turns a monotonic
 * deadline into a relative wait, and a semaphore is a pipe.
 * _CCOL_EMULATE_DARWIN_SYNC selects the same code on any other system, and
 * that's how the Linux and FreeBSD test suites run it; there the relative
 * wait is the absolute one of the realtime clock. Only the library defines
 * it, never an application. */
#if defined(__APPLE__) && !defined(_CCOL_EMULATE_DARWIN_SYNC)
#define _CCOL_EMULATE_DARWIN_SYNC 1
#endif

/* The close-on-exec gate. Where a descriptor can't be created close-on-exec
 * in one call (a socket, an accepted socket, a socket pair and a pipe on
 * macOS), the library creates it first and sets FD_CLOEXEC afterwards. A
 * fork() from another thread between those two steps hands the child the
 * descriptor without the flag, and an exec() in that child keeps it open.
 * So both steps happen between _ccol_cloexec_gate_enter() and
 * _ccol_cloexec_gate_leave(), which take the read side of a process-wide
 * lock, while a fork-prepare handler takes the write side, and a fork()
 * therefore waits until no descriptor sits between the two steps.
 * posix_spawn(3) runs no fork handler, so a program that spawns a child on
 * macOS while the library creates a socket uses POSIX_SPAWN_CLOEXEC_DEFAULT.
 * The gate only exists where the two steps exist, so on Linux and FreeBSD it
 * costs nothing. The two functions are internal and not exported. */
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
#define _ccol_mutex_t pthread_mutex_t

/** @brief Destroy a mutex */
#define _ccol_mutex_destroy(m) pthread_mutex_destroy(&(m))

/** @brief Initialize a mutex with default attributes */
#define _ccol_mutex_init(m) pthread_mutex_init(&(m), NULL)

/** @brief Lock a mutex. The call blocks. */
#define _ccol_mutex_lock(m) pthread_mutex_lock(&(m))

/** @brief Unlock a mutex */
#define _ccol_mutex_unlock(m) pthread_mutex_unlock(&(m))

/**
 * @brief Lock a mutex only when no other thread holds it. It returns 0 when
 *        it got the lock and EBUSY when another thread holds it, as the
 *        wrapped call does, and it never waits.
 */
#define _ccol_mutex_trylock(m) pthread_mutex_trylock(&(m))

/**
 * @brief Lock a mutex, or give up at an absolute CLOCK_REALTIME time t (a
 *        struct timespec). Returns 0 or ETIMEDOUT, as the wrapped call
 *        does.
 */
#if defined(_CCOL_EMULATE_DARWIN_SYNC)
#define _ccol_mutex_timedlock(m, t) _ccol_mutex_timedlock_poll(&(m), &(t))
#else
#define _ccol_mutex_timedlock(m, t) pthread_mutex_timedlock(&(m), &(t))
#endif

/** @brief Read-write lock type (wraps pthread_rwlock_t) */
#define _ccol_rw_lock_t pthread_rwlock_t

/** @brief Destroy a read-write lock */
#define _ccol_rw_lock_destroy(a) pthread_rwlock_destroy(&(a))

/** @brief Initialize a read-write lock with default attributes */
#define _ccol_rw_lock_init(a) pthread_rwlock_init(&(a), NULL)

/**
 * @brief Initialize again, in the child of fork(), a read-write lock that the
 *        forking thread held
 *
 * The child has one thread so nothing waits on the lock, but a plain unlock
 * doesn't always release it there (glibc compares the writer's thread ID,
 * and the child's thread has a new one). macOS refuses with EBUSY to
 * initialize an object it still sees as a lock in use, so the object gets
 * cleared first; on FreeBSD the lock is a pointer to a block that the init
 * replaces either way. The forking thread must hold the write lock, which
 * is how a fork-prepare handler leaves it. Gives 0 or an error number.
 */
#define _ccol_rw_lock_reinit_in_child(a) \
  _ccol_rw_lock_reinit_in_child_impl(&(a))
/* The parameter is void * because under a strict -std=c11 <pthread.h>
 * declares no rwlock type, and a header that only includes this one still
 * has to compile. */
#pragma GCC visibility push(hidden)
int _ccol_rw_lock_reinit_in_child_impl(void *lock);
#pragma GCC visibility pop

/** @brief Take the write lock (exclusive access) */
#define _ccol_rw_lock_wrlock(a) pthread_rwlock_wrlock(&(a))

/** @brief Take the read lock (shared access) */
#define _ccol_rw_lock_rdlock(a) pthread_rwlock_rdlock(&(a))

/**
 * @brief Take the write lock, or give up at an absolute CLOCK_REALTIME time t
 *        (a struct timespec). Returns 0 or ETIMEDOUT, as the wrapped call
 *        does.
 */
#if defined(_CCOL_EMULATE_DARWIN_SYNC)
#define _ccol_rw_lock_timedwrlock(a, t) \
  _ccol_rw_lock_timedwrlock_poll(&(a), &(t))
#else
#define _ccol_rw_lock_timedwrlock(a, t) pthread_rwlock_timedwrlock(&(a), &(t))
#endif

/** @brief Unlock a read-write lock */
#define _ccol_rw_lock_unlock(a) pthread_rwlock_unlock(&(a))

#if !defined(_CCOL_EMULATE_DARWIN_SYNC)
/** @brief Condition variable type (wraps pthread_cond_t) */
#define _ccol_cond_var_t pthread_cond_t

/** @brief Condition variable attributes type (wraps pthread_cond_attr_t) */
#define _ccol_cond_var_attr_t pthread_condattr_t

/** @brief Destroy condition variable attributes */
#define _ccol_cond_var_attr_destroy(ca) pthread_condattr_destroy(&(ca))

/** @brief Initialize condition variable attributes */
#define _ccol_cond_var_attr_init(ca) pthread_condattr_init(&(ca))

/** @brief Set the clock type of condition variable attributes */
#define _ccol_cond_var_attr_setclock(ca, clk) \
  pthread_condattr_setclock(&(ca), clk)

/** @brief Destroy a condition variable */
#define _ccol_cond_var_destroy(c) pthread_cond_destroy(&(c))

/** @brief Initialize a condition variable with default attributes */
#define _ccol_cond_var_init(c) pthread_cond_init(&(c), NULL)

/** @brief Initialize a condition variable with custom attributes */
#define _ccol_cond_var_init_ca(c, ca) pthread_cond_init(&(c), &(ca))

/** @brief Wait on a condition variable. The mutex is unlocked while waiting. */
#define _ccol_cond_var_wait(c, m) pthread_cond_wait(&(c), &(m))

/** @brief A timed wait on a condition variable, with an absolute timeout */
#define _ccol_cond_var_timedwait(c, m, t) \
  pthread_cond_timedwait(&(c), &(m), &(t))

/** @brief Signal one thread that waits on a condition variable */
#define _ccol_cond_var_signal(c) pthread_cond_signal(&(c))

/** @brief Signal every thread that waits on a condition variable */
#define _ccol_cond_var_broadcast(c) pthread_cond_broadcast(&(c))
#else
/* A condition variable that remembers the clock of its deadlines; see
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

#define _ccol_cond_var_t _ccol_cond_var_s
#define _ccol_cond_var_attr_t _ccol_cond_var_attr_s
#define _ccol_cond_var_attr_destroy(ca) \
  _ccol_cond_var_attr_set(&(ca), (ca).clock)
#define _ccol_cond_var_attr_init(ca) \
  _ccol_cond_var_attr_set(&(ca), CLOCK_REALTIME)
#define _ccol_cond_var_attr_setclock(ca, clk) \
  _ccol_cond_var_attr_set(&(ca), (clk))
#define _ccol_cond_var_destroy(c) pthread_cond_destroy(&(c).cond)
#define _ccol_cond_var_init(c) _ccol_cond_var_init_clock(&(c), CLOCK_REALTIME)
#define _ccol_cond_var_init_ca(c, ca) \
  _ccol_cond_var_init_clock(&(c), (ca).clock)
#define _ccol_cond_var_wait(c, m) pthread_cond_wait(&(c).cond, &(m))
#define _ccol_cond_var_timedwait(c, m, t) \
  _ccol_cond_var_timedwait_clock(&(c), &(m), &(t))
#define _ccol_cond_var_signal(c) pthread_cond_signal(&(c).cond)
#define _ccol_cond_var_broadcast(c) pthread_cond_broadcast(&(c).cond)
#endif

/**
 * @brief Unnamed, process-private semaphore type (wraps sem_t)
 *
 * This is the one synchronization primitive in this file that POSIX
 * guarantees to be async-signal-safe, through _ccol_semaphore_post() below,
 * which uses sem_post(3). None of the others (_ccol_mutex_t,
 * _ccol_cond_var_t, _ccol_rw_lock_t, _ccol_thread_create() and
 * _ccol_call_once()) is safe to call inside a signal handler.
 *
 * pthread_mutex_lock can deadlock against itself when the interrupted
 * thread already holds the very mutex the handler tries to lock, and
 * pthread_create and pthread_once can need malloc's arena lock, which the
 * interrupted thread may already hold for some unrelated reason.
 *
 * When a module needs an entry point that is truly async-signal-safe
 * ("safe to call from a SIGTERM handler", say), don't call
 * _ccol_mutex_lock, _ccol_thread_create or _ccol_call_once from it. Wake a
 * dedicated watcher thread that is already running with
 * _ccol_semaphore_post() instead; that thread is an ordinary execution
 * context and not a signal context, so it can do the real work. The
 * engine-stop watcher in chttpserver.c is the reference implementation of
 * this pattern.
 */
#if !defined(_CCOL_EMULATE_DARWIN_SYNC)
#define _ccol_semaphore_t sem_t

/** @brief Initialize an unnamed, process-private semaphore whose count
 *  starts at value. */
#define _ccol_semaphore_init(s, value) sem_init(&(s), 0, (value))

/** @brief Destroy a semaphore initialized with _ccol_semaphore_init(). */
#define _ccol_semaphore_destroy(s) sem_destroy(&(s))

/** @brief Block until the semaphore's count is > 0, then decrement it
 *  atomically. It can block, so it is not async-signal-safe: call it only
 *  from an ordinary thread, never inside a signal handler. */
#define _ccol_semaphore_wait(s) sem_wait(&(s))

/**
 * @brief Increment the semaphore's count, which wakes one waiter if there
 *        is one.
 *
 * POSIX makes sem_post(3) async-signal-safe, so this is the one operation in
 * this file you can call inside a signal handler.
 */
#define _ccol_semaphore_post(s) sem_post(&(s))
#else
/* A semaphore made out of a pipe, where each byte in the pipe is one unit of
 * the count. write(2) is async-signal-safe, so a post keeps the guarantee
 * the semaphore is there for. The write end doesn't block, so a post never
 * waits, just like sem_post(3) never does: once the count fills the pipe a
 * post leaves the count as it is and still succeeds. That's fine because
 * every user of the semaphore in this library sets a request flag before
 * its post and the waiter reads every flag after each wake, so a count the
 * pipe can't hold carries no request of its own; the posts already in the
 * pipe wake the waiter at least once more after the flag is set. */
typedef struct {
  int fd[2];
} _ccol_semaphore_s;
#define _ccol_semaphore_t _ccol_semaphore_s
#define _ccol_semaphore_init(s, value) _ccol_semaphore_pipe_init(&(s), (value))
#define _ccol_semaphore_destroy(s) _ccol_semaphore_pipe_destroy(&(s))
#define _ccol_semaphore_wait(s) _ccol_semaphore_pipe_wait(&(s))
#define _ccol_semaphore_post(s) _ccol_semaphore_pipe_post(&(s))
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

/* One pause of a try-lock loop, 20 microseconds at first and doubling up to
 * 1 millisecond, so a short wait is cheap and a long one doesn't eat CPU. */
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

/* A timed wait whose deadline t is on the clock that c remembers. A
 * monotonic deadline is turned into the time left, which macOS waits for
 * with pthread_cond_timedwait_relative_np(); anywhere else (a test build)
 * the time left is added to the realtime clock. Gives 0 or ETIMEDOUT. */
static inline int _ccol_cond_var_timedwait_clock(_ccol_cond_var_s *c,
                                                 pthread_mutex_t *m,
                                                 const struct timespec *t) {
  if (c->clock != CLOCK_MONOTONIC)
    return pthread_cond_timedwait(&c->cond, m, t);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  /* The time left, worked out field by field, since a deadline at the end
   * of time (a saturated one) would overflow a count of nanoseconds. A wait
   * longer than 10^9 seconds is cut down to that; every caller waits in a
   * loop that checks its condition, so an early wake just waits again. */
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
  /* The sum saturates at the largest time_t. A 32-bit time_t gets there well
   * inside the 10^9 seconds above, and a wrapped sum would be in the past
   * and end every wait at once. */
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

/* sem_init(): a pipe holding value bytes, with a write end that doesn't
 * block. Gives 0, or -1 with errno set, like sem_init() does; a value that
 * the pipe can't hold fails with EINVAL. */
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

static inline int _ccol_semaphore_pipe_destroy(_ccol_semaphore_s *s) {
  close(s->fd[0]);
  close(s->fd[1]);
  return 0;
}

/* sem_wait(): every failure gives -1 with errno set. The end of the pipe
 * and a descriptor that isn't open both mean the semaphore isn't valid, so
 * they give EINVAL, as sem_wait() does for such a semaphore. */
static inline int _ccol_semaphore_pipe_wait(_ccol_semaphore_s *s) {
  char b;
  for (;;) {
    ssize_t n = read(s->fd[0], &b, 1);
    if (n == 1) return 0;
    if (n < 0 && errno == EINTR) continue;
    if (n >= 0 || errno == EBADF) errno = EINVAL;
    return -1;
  }
}

/* write(2) is async-signal-safe and so is this. A full pipe (EAGAIN) counts
 * as success, see the comment of _ccol_semaphore_s. errno keeps its value on
 * success, so a post from a signal handler doesn't change the errno of the
 * thread it interrupts. */
static inline int _ccol_semaphore_pipe_post(_ccol_semaphore_s *s) {
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
#endif /* _CCOL_EMULATE_DARWIN_SYNC */

/** @brief Thread ID type (wraps pthread_t) */
#define _ccol_thread_id_t pthread_t

/** @brief Get current thread ID */
#define _ccol_get_thread_id pthread_self

/** @brief Nonzero when two thread IDs name the same thread (wraps
 *         pthread_equal) */
#define _ccol_thread_id_equal(a, b) pthread_equal((a), (b))

/**
 * @brief Create a thread that runs fn(arg), and store its handle in handle
 *
 * The attributes argument is hidden because every call site passes the
 * default attributes (NULL) and only reads the return value as success or
 * failure. _ccol_mutex_init() and _ccol_cond_var_init() hide their own NULL
 * attributes argument the same way.
 */
#define _ccol_thread_create(handle, fn, arg) \
  pthread_create(&(handle), NULL, (fn), (arg))

/** @brief Block until the thread that handle names stops */
#define _ccol_thread_join(handle) pthread_join((handle), NULL)

/**
 * @brief Examine or change the signal mask of the calling thread (wraps
 *        pthread_sigmask)
 *
 * how is SIG_BLOCK, SIG_UNBLOCK or SIG_SETMASK, and set and oldset are
 * sigset_t pointers, either of which can be NULL, exactly as for
 * pthread_sigmask(). Gives 0 on success and an error number on failure. The
 * translation unit that uses it includes <signal.h>.
 */
#define _ccol_thread_sigmask(how, set, oldset) \
  pthread_sigmask((how), (set), (oldset))

/**
 * @brief Send the signal sig to the thread that handle names (wraps
 *        pthread_kill)
 *
 * Gives 0 on success and an error number on failure. The translation unit
 * that uses it includes <signal.h>.
 */
#define _ccol_thread_kill(handle, sig) pthread_kill((handle), (sig))

/** @brief One-time-initialization guard type (wraps pthread_once_t) */
#define _ccol_once_flag_t pthread_once_t

/**
 * @brief Static initializer for a _ccol_once_flag_t (wraps PTHREAD_ONCE_INIT)
 *
 * A one-time-init flag only holds a simple "not run yet" state that every
 * platform can represent; it isn't a real synchronization object like a
 * mutex, a condition variable or a rwlock. That's why it is the one
 * primitive in this file allowed a static, constant initializer, and it's
 * also what lets the library initialize everything else lazily without a
 * chicken-and-egg problem.
 */
#define _CCOL_ONCE_INIT PTHREAD_ONCE_INIT

/** @brief Run fn exactly once for all the callers that race on the same
 *  flag */
#define _ccol_call_once(flag, fn) pthread_once(&(flag), (fn))

/** @brief Thread-local storage key type (wraps pthread_key_t) */
#define _ccol_thread_ls_key_t pthread_key_t

/** @brief Create a thread-local storage key with an optional destructor */
#define _ccol_thread_ls_key_create(key, destructor) \
  pthread_key_create(&(key), (destructor))

/** @brief Delete a thread-local storage key */
#define _ccol_thread_ls_key_delete(key) pthread_key_delete((key))

/** @brief Set the calling thread's value for a thread-local storage key */
#define _ccol_thread_ls_set(key, val) pthread_setspecific((key), (val))

/** @brief Get the calling thread's value for a thread-local storage key */
#define _ccol_thread_ls_get(key) pthread_getspecific((key))

/** @brief Register prepare, parent and child handlers that run around
 *  fork(2)
 *
 * Where the close-on-exec gate exists, the gate's handlers are registered
 * first, once. Prepare handlers run in the reverse order of registration,
 * so the gate then waits after every other prepare handler of the library,
 * whichever of them registers first. */
#define _ccol_at_fork_raw(prepare, parent, child) \
  pthread_atfork((prepare), (parent), (child))
#if defined(_CCOL_CLOEXEC_GATE)
#define _ccol_at_fork_first() _ccol_cloexec_gate_register()
#endif
#if defined(_ccol_at_fork_first)
#define _ccol_at_fork(prepare, parent, child) \
  (_ccol_at_fork_first(), _ccol_at_fork_raw(prepare, parent, child))
#else
#define _ccol_at_fork(prepare, parent, child) \
  _ccol_at_fork_raw(prepare, parent, child)
#endif

/**
 * @brief Get a thread's name (wraps pthread_getname_np)
 *
 * Not portable, only Apple and the BSDs have it. Only the platform fallback
 * branches in clogger.c use it, for a system that can't look a thread name
 * up through /proc. OpenBSD calls it pthread_get_name_np(), declares it in
 * <pthread_np.h> and gives no result, so there the wrapper reports success.
 */
#if defined(__OpenBSD__)
#define _ccol_get_thread_name_np(thread, buf, len) \
  (pthread_get_name_np((thread), (buf), (len)), 0)
#else
#define _ccol_get_thread_name_np(thread, buf, len) \
  pthread_getname_np((thread), (buf), (len))
#endif

/**
 * @brief Rename the calling thread (wraps pthread_setname_np)
 *
 * Linux renames a thread through prctl(PR_SET_NAME) instead, so only the
 * platform branches of ccol_set_thread_name() in clogger.c use this. macOS
 * can only rename the calling thread and its pthread_setname_np takes just
 * the name, while FreeBSD takes the thread too. OpenBSD has
 * pthread_set_name_np() in <pthread_np.h>, which gives no result.
 */
#if defined(__APPLE__)
#define _ccol_set_own_thread_name_np(name) pthread_setname_np((name))
#elif defined(__OpenBSD__)
#define _ccol_set_own_thread_name_np(name) \
  (pthread_set_name_np(pthread_self(), (name)), 0)
#else
#define _ccol_set_own_thread_name_np(name) \
  pthread_setname_np(pthread_self(), (name))
#endif

/**
 * @brief Get a thread's 64-bit numeric id (wraps pthread_threadid_np)
 *
 * Not portable, only Apple has it. Only the macOS branch of the thread id
 * lookup in clogger.c uses it.
 */
#define _ccol_get_thread_id_np(thread, out_id) \
  pthread_threadid_np((thread), (out_id))

#if defined(__FreeBSD__)
/**
 * @brief Get the kernel thread id of the calling thread (wraps
 *        pthread_getthreadid_np)
 *
 * FreeBSD declares the call in <pthread_np.h>, which the caller includes.
 * Only the FreeBSD branch of the thread id lookup in clogger.c uses it.
 */
#define _ccol_get_own_thread_os_id_np() pthread_getthreadid_np()
#endif

/**
 * @brief Assertion macro for collections library
 *
 * @param cond Condition that must hold
 *
 * We don't use the standard library's assert() here, on purpose. The
 * library documents a "will assert on misuse" contract in lots of places:
 * double-free detection, corruption detection, rejecting a foreign pointer
 * or a pointer from another container, and every other invariant this macro
 * guards in every module. Those are load-bearing safety checks that a caller
 * can depend on, and a release build must not compile them away.
 *
 * A plain assert() quietly turns into a no-op once NDEBUG is defined, so
 * every one of those checks would stop aborting and the program would carry
 * on with a state that is known to be invalid, for example dereferencing a
 * null pointer one line later or a double free that nobody catches. Those
 * are exactly the outcomes the checks are there to prevent.
 *
 * This macro always evaluates cond and always stops the program with
 * abort() when cond is false. abort() raises SIGABRT, the same signal a
 * failed standard assert() raises, and neither NDEBUG nor any other
 * compilation setting changes any of this.
 */
#define _ccol_assert(cond)                                                   \
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
#define _ccol_stringify(s) #s

/**
 * @brief Expand and stringify macro argument (internal helper)
 * @param s Macro to expand then stringify
 */
#define _ccol_x_stringify(s) _ccol_stringify(s)

/* Internal. Gives an identifier that no other expansion in the translation
 * unit can give: prefix, an underscore and the value of n. The container
 * macros pass __COUNTER__ as n and name all their temporaries this way. A
 * temporary is in scope inside its own initializer, and that's where the
 * caller's argument is evaluated, so a public macro nested in the argument
 * of another public macro or of itself (like chmap_get(m, chmap_get(m, k)))
 * would otherwise declare a second local of the same name there. -Wshadow
 * reports that, and a caller's -Werror build fails. The two levels make n
 * expand before the paste. */
#define _ccol_uniq(prefix, n) _ccol_uniq_paste(prefix, n)
#define _ccol_uniq_paste(prefix, n) prefix##_##n

/* Internal. One _Generic association, with its trailing comma, mapping the
 * type of the C23 nullptr constant to v in a translation unit that has
 * nullptr, and nothing otherwise. A classifying _Generic puts it among its
 * associations so that nullptr picks the same result as a NULL of type
 * void *. GCC 13 and Clang 16 are the first releases with nullptr, and they
 * report a __STDC_VERSION__ below 202311L for -std=c2x. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L && \
    ((defined(__clang__) && __clang_major__ >= 16) ||          \
     (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 13))
#define _CCOL_NULLPTR_ASSOC(v) __typeof__(nullptr) : v,
#else
#define _CCOL_NULLPTR_ASSOC(v)
#endif

/* Internal. Defined when the compiler is a Clang that knows the
 * -Wunreachable-code-generic-assoc warning. That warning reports the
 * associations of a _Generic that a qualified type can never pick, and the
 * classifying macros of these headers list those on purpose, so they have a
 * second form for such a compiler. A Clang without the warning takes the
 * plain form, since a pragma naming a warning it doesn't know is a
 * -Wunknown-warning-option diagnostic in its own right. */
#if defined(__clang__) && defined(__has_warning)
#if __has_warning("-Wunreachable-code-generic-assoc")
#define _CCOL_CLANG_GENERIC_ASSOC_WARNING 1
#endif
#endif

/**
 * @brief The ownership rule for every `err` and `err_str` out-parameter in
 *        this library.
 *
 * THE LIBRARY OWNS THE STRING. A CALLER MUST NEVER FREE IT.
 *
 * The rule is the same in every module. Most of them hand back a
 * _CCOL_ERR_STR() literal, which has static storage duration and lives for
 * the whole process. cjson and cyaml build a message that names a position
 * and a token instead, so their text isn't known until the parse fails, and
 * they hand back a pointer into a per-thread buffer the module owns. Both
 * kinds are library storage and free() is wrong for both.
 *
 * The lifetimes differ, and only the second kind has one worth naming. A
 * literal is valid forever, while a buffered message is valid until the
 * next FAILING call of the same kind on the SAME thread, which is the
 * lifetime strerror(3) and dlerror(3) give. Copy the text if you need it
 * past that point; each such function's doc comment says it again where you
 * use it.
 *
 * An out-parameter that a caller DOES own is never called err. It's always
 * a return value or an explicitly named buffer, and its own doc comment
 * names the function that frees it.
 */

/**
 * @brief Create error string with file and line information
 *
 * Internal. Every literal err string the library hands back is built with
 * this macro.
 *
 * It makes a compile-time error string that holds the source file, the
 * line number and a custom error message.
 *
 * @param x Error message string
 * @return String literal: "file:line - message"
 *
 * Example:
 * @code
 * char *err = _CCOL_ERR_STR("failed to allocate memory");
 * // Results in: "myfile.c:42 - failed to allocate memory"
 * @endcode
 */
#define _CCOL_ERR_STR(x) (__FILE__ ":" _ccol_x_stringify(__LINE__) " - " x)

/**
 * @brief Fatal error macro - print message and assert (internal)
 *
 * Formats an error message to stderr and then fails an assertion. The
 * library calls it, and the type-safe macros expand into it, for an error
 * the program can't recover from. It isn't part of the API; an application
 * reports its own errors its own way.
 *
 * @param _err_fmt Printf-style format string
 * @param ... Format arguments
 *
 * @note It always stops the program with _ccol_assert(false)
 * @note The message goes out in one fprintf() call to stderr, prefixed with
 * the file and the line, so it has no length limit
 *
 */
#define _ccol_fatal_err(...) _ccol_fatal_err_impl(__VA_ARGS__, "")

/* The public macro appends one empty string to the caller's arguments and
 * the trailing %s of the format prints it. That way the list after the
 * format string is never empty, and we need neither the GNU comma swallow
 * (, ##__VA_ARGS__) nor at least one argument after the format. Both are
 * extensions that clang -pedantic and gcc -pedantic reject under -std=c11,
 * and the inline functions of the installed headers expand this macro, so a
 * strict build that just includes them would fail. */
#define _ccol_fatal_err_impl(_err_fmt, ...)                               \
  do {                                                                    \
    fprintf(stderr, "%s:%d: fatal: " _err_fmt "%s\n", __FILE__, __LINE__, \
            __VA_ARGS__);                                                 \
    _ccol_assert(false);                                                  \
  } while (0)

/* ========================================================================== */
/*                         MEMORY MANAGEMENT                                  */
/* ========================================================================== */

/**
 * @brief Allocate memory (default: malloc). Internal; the
 *        allocator that _ccol_mem_*() uses when no procs are given.
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
#define _ccol_default_mem_alloc(size) malloc((size))

/**
 * @brief Allocate and zero-initialize memory (default: calloc). Internal; the
 *        allocator that _ccol_mem_*() uses when no procs are given.
 * @param elem_count Number of elements
 * @param elem_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
#define _ccol_default_mem_calloc(elem_count, elem_size) \
  calloc((elem_count), (elem_size))

/**
 * @brief Reallocate memory (default: realloc). Internal; the
 *        allocator that _ccol_mem_*() uses when no procs are given.
 * @param ptr Existing pointer to reallocate
 * @param new_size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
#define _ccol_default_mem_realloc(ptr, new_size) realloc((ptr), (new_size))

/**
 * @brief Free memory (default: free). Internal; the
 *        allocator that _ccol_mem_*() uses when no procs are given.
 * @param ptr Pointer to free
 */
#define _ccol_default_mem_free(ptr) free((ptr))

/**
 * @brief Allocate memory with the custom allocator or the default one
 * @param m_procs Memory management procedures (or NULL for default)
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
/* The outer parentheses are load-bearing. ?: binds more loosely than every
 * arithmetic operator, relational operator and cast operator, so a body with
 * no parentheses lets an operator outside the macro bind to one arm only.
 * Without them, `(char *)_ccol_mem_alloc(mp, n) + off` parses as
 * `(char *)(mp) ? (mp)->malloc(n) : malloc(n) + off`, which applies the cast
 * to m_procs and the offset to the arm that the code does not take. Because
 * both arms are void * and -Wpointer-arith is in neither -Wall nor -Wextra,
 * the wrong code compiles with no report. The same is true for every sibling
 * macro below. */
#define _ccol_mem_alloc(m_procs, size) \
  ((m_procs) ? (m_procs)->malloc((size)) : _ccol_default_mem_alloc((size)))

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
             : _ccol_default_mem_calloc((e_count), (e_size)))

/**
 * @brief Reallocate memory with the custom allocator or the default one
 * @param m_procs Memory management procedures (or NULL for default)
 * @param ptr Existing pointer
 * @param new_size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
#define _ccol_mem_realloc(m_procs, ptr, new_size)    \
  ((m_procs) ? (m_procs)->realloc((ptr), (new_size)) \
             : _ccol_default_mem_realloc((ptr), (new_size)))

/**
 * @brief Free memory with the custom allocator or the default one
 * @param m_procs Memory management procedures (or NULL for default)
 * @param ptr Pointer to free
 */
#define _ccol_mem_free(m_procs, ptr) \
  ((m_procs) ? (m_procs)->free((ptr)) : _ccol_default_mem_free((ptr)))

/**
 * @brief The invalid size_t for operations that use a size
 *
 * It's the largest value a size_t can hold
 */
#define ccol_invalid_size ((size_t)-1)

/**
 * @brief The maximum power of two that can be stored in a size_t
 *        (internal; ccol_max_elem_count is the public name of the limit)
 *
 * on 32 bit archs -> 2^31
 *
 * on 64 bit archs -> 2^63
 *
 * 2^(#arch_bits) doesn't fit in a size_t, so the largest integer power of
 * two a size_t variable can hold is half of 2^(#arch_bits).
 */
#define _ccol_max_power_of_two_size_t \
  ((size_t)1 << ((sizeof(size_t) * CHAR_BIT) - 1))

/**
 * @brief Maximum element count for collections
 *
 * The largest power of 2 a size_t can hold
 */
#define ccol_max_elem_count _ccol_max_power_of_two_size_t

/* ========================================================================== */
/*                         RETURN VALUE CODES                                 */
/* ========================================================================== */

/**
 * @brief Standard return codes for collection operations
 *
 * Every collection function returns one of these codes to say whether it
 * worked or what exactly went wrong: a negative value is an error and zero
 * is success.
 */
typedef enum ccollections_retval_t {
  /* Every enumerator below has an explicit value on purpose, even though C
   * would give the same values if they were implicit. ccol_success == 0 is a
   * load-bearing invariant that real call sites depend on, and some of them
   * compare "== 0" directly instead of writing ccol_success. With implicit
   * values, inserting a new enumerator anywhere but at the very end quietly
   * renumbers every later entry, ccol_success included.
   *
   * You can't see such a renumbering where the edit is made. It shows up far
   * away, as a test of an unrelated module failing on a retval it never
   * touched, so every value is pinned explicitly and a future addition can't
   * do that. */
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

/** Returns a string literal for @p r, handy in a _ccol_fatal_err() message.
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
 * Writes a hex dump of @p size bytes at @p data to stderr in an xxd-like
 * format: an offset, hex columns and a sidebar of printable ASCII. The cbmap
 * and chmap macros call it by themselves right before a _ccol_fatal_err()
 * for a failed insert or lookup, so you can see the key that caused the
 * failure even when it's opaque binary data.
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
 * Uses the cleanup attribute of GCC and Clang, which calls destructor when
 * the variable goes out of scope.
 *
 * @param destructor Function to call with pointer to variable
 *
 * Example:
 * @code
 * void cleanup_int(int **p) { free(*p); *p = NULL; }
 * int *ptr _ccol_destructor(cleanup_int) = malloc(sizeof(int));
 * // ptr is cleaned up automatically at the end of the scope
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
 * Holds the custom functions that allocate and free memory. Give it to a
 * collection to create that collection with custom memory management.
 *
 * @note All four function pointers must be non-NULL when you give this struct
 * @note The functions must behave like the standard malloc, free, calloc and
 * realloc
 * @note That includes alignment. Every block that malloc, calloc and realloc
 * return must be aligned for any object type (_Alignof(max_align_t)),
 * exactly as the standard functions guarantee. The containers put entries
 * with that alignment at the start of a block, so a less aligned block makes
 * every access to such an entry undefined behavior, and a pool or arena
 * allocator that hands out blocks at a smaller granularity must round
 * each block up to that alignment.
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
 * Used for a sort and for an ordered map. The function must return:
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
 * Used for a hash map. The function returns a hash value for the data it
 * gets, and a good one spreads the values uniformly.
 *
 * @param ptr Pointer to data to hash
 * @param size Size in bytes of the data at ptr
 * @return Hash value (unsigned long)
 *
 * @note For a key type of a fixed size, size is the map's fixed key size,
 * which comes from the key type the map was constructed with. For a binary
 * key of variable length (a string, or another buffer-like key) size is the
 * byte length the caller gives. So a hash function can always hash exactly
 * ptr[0..size) and never needs an outside convention, like a length prefix
 * or a NUL terminator, to know where to stop.
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
 * A hash map uses this together with a ccol_hashing_proc_t to decide whether
 * two keys are the same key; it returns true when they are equal.
 *
 * @param first Pointer to the first key
 * @param first_size Size in bytes of the key at first
 * @param second Pointer to the second key
 * @param second_size Size in bytes of the key at second
 * @return true when the two keys are the same key, and false otherwise
 *
 * @note It must agree with the hash function the map uses: two keys it
 * reports as equal must get the same hash. A pair it calls equal but that
 * hashes differently can land in different buckets, and a lookup then
 * misses a key that is present.
 * @note first and second carry no alignment for the key type, so read a
 * field through memcpy, or through a pointer to a type whose alignment is 1.
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
 * A pointer and a size, which every map (chmap, cbmap) uses to carry a key
 * or a value of any type and any size.
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
 * The iterator struct every map uses, which gives access to the current
 * key-value pair of an iteration.
 *
 * @note The pointers stay valid until somebody changes the map
 * @note You must destroy the iterator at the end, or let it go out of scope
 */
typedef struct cmap_iterator {
  /* Both pairs point at the container's accessor for the current entry. For
   * cbstmap that's the node's authoritative pair, and for chashmap it's what
   * every later lookup of the key reports. You get an iterator so you can
   * read them and write the bytes ptr addresses within size, but assigning
   * to ptr or to size themselves is a compile error, since the two describe
   * each other. Without the qualifier a caller going around
   * ccol_iter_key_ptr() and ccol_iter_val_ptr() could store a pointer that
   * the container never allocated, and cbstmap's teardown would then free
   * it. */
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
 * When the caller gives a custom memory management struct, checks that
 * every needed function pointer in it is non-NULL.
 *
 * @param mmgt_procs The memory management procs to verify (or NULL)
 * @param err An optional pointer that receives an error string
 * @return true if the procs are valid or NULL, false if they are invalid
 *
 * @note A NULL mmgt_procs gives true, and the library then uses the default
 * malloc and free
 * @note All four functions must be given when the struct is non-NULL
 */
#define _ccol_verify_memmgmt_procs(mmgt_procs, err)                        \
  ({                                                                       \
    bool __ccol_vmp_ok = true;                                             \
    if ((mmgt_procs) && (!(mmgt_procs)->malloc || !(mmgt_procs)->calloc || \
                         !(mmgt_procs)->realloc || !(mmgt_procs)->free)) { \
      if ((err)) {                                                         \
        *(err) = _CCOL_ERR_STR(                                            \
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
 * into the container struct; it's used when a container gets created.
 *
 * @param container The container struct to populate
 * @param mmgmt_procs The source memory management procs (or NULL)
 * @param err An optional pointer that receives an error string
 * @return true for a success, false if the allocation fails
 *
 * @note A NULL mmgmt_procs sets container->m_procs to NULL, and the
 * container uses the default procs
 * @note The memory for m_procs comes from the allocator the caller gives
 */
#define _ccol_populate_mem_mgmt_procs(container, mmgmt_procs, err) \
  ({                                                               \
    bool __ccol_pmp_ok = true;                                     \
    if ((mmgmt_procs)) {                                           \
      (container)->m_procs =                                       \
          (mmgmt_procs)->malloc(sizeof(ccol_memmgmt_procs_t));     \
      if (!(container)->m_procs) {                                 \
        if ((err)) {                                               \
          *(err) = _CCOL_ERR_STR(                                  \
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
 * Don't construct this struct yourself, the scoped_ptr macros fill in every
 * field. It's only there so the cleanup callback can find two things at the
 * end of the scope: the allocator that frees the pointer, and the
 * pointer's current value, which a later call (to _ccol_mem_alloc, for
 * example) may have changed after the declaration.
 */
typedef struct _ccol_scoped_ptr_ctx_t {
  void **ptr_addr;               /**< Address of the guarded pointer var */
  ccol_memmgmt_procs_t *m_procs; /**< Allocator to free it with, or NULL */
} _ccol_scoped_ptr_ctx_t;

/**
 * @brief Cleanup callback bound by ccol_scoped_ptr / ccol_scoped_ptr_mp
 * @param ctx Address of the hidden companion variable the macros declare
 */
static inline void _ccol_scoped_ptr_cleanup(_ccol_scoped_ptr_ctx_t *ctx) {
  if (ctx->ptr_addr && *ctx->ptr_addr) {
    _ccol_mem_free(ctx->m_procs, *ctx->ptr_addr);
    *ctx->ptr_addr = NULL;
  }
}

/**
 * @brief Declare a raw pointer that is freed automatically at scope exit
 *
 * Declares `type *name` and sets it to NULL. Assign to it the usual way,
 * for example with mmgmt_procs->malloc(size), or with a plain malloc() when
 * mmgmt_procs is NULL. At the end of the enclosing scope whatever value name
 * holds is freed with mmgmt_procs->free, or with free() when mmgmt_procs is
 * NULL.
 *
 * @param name The name of the pointer variable to declare
 * @param type The type that the pointer points to (for example char,
 *             struct foo)
 * @param mmgmt_procs The custom allocator that frees the last value of name,
 *                     or NULL for the default allocator
 *
 * @note mmgmt_procs is kept by reference, not copied, so it must stay
 *       valid for at least as long as the enclosing scope of name; that's
 *       naturally the case when mmgmt_procs is a variable already in scope
 *       at the point of the declaration
 * @note Only the last value of name is freed, so free an earlier value
 *       yourself before you give name a new value inside the scope, as with
 *       every other guard built on the cleanup attribute
 * @note It expands to two declarations rather than one expression, so use
 *       it as a statement of its own; every *_construct_scoped macro in this
 *       library has the same constraint
 * @note Use ccol_scoped_ptr_release() to hand ownership to code outside the
 *       enclosing scope, so that name isn't freed when the scope ends
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
/* The hidden context variable is only there for its cleanup handler and no
 * code names it. It's marked unused because Clang 11 doesn't count a
 * cleanup handler as a use and would report it under -Wunused-variable. The
 * attribute covers this variable alone, so an unused variable of the caller
 * still gets reported. */
#define ccol_scoped_ptr_mp(name, type, mmgmt_procs)                      \
  type *name = NULL;                                                     \
  _ccol_scoped_ptr_ctx_t name##__ccol_scoped_ctx __attribute__((unused)) \
  _ccol_destructor(_ccol_scoped_ptr_cleanup) = {(void **)&(name),        \
                                                (mmgmt_procs)}

/**
 * @brief Declare a raw pointer that the default allocator frees
 *        automatically at scope exit
 *
 * Same as ccol_scoped_ptr_mp(name, type, NULL); see that macro for the full
 * contract.
 */
#define ccol_scoped_ptr(name, type) ccol_scoped_ptr_mp(name, type, NULL)

/**
 * @brief Release the ownership of a scoped pointer, so that nothing frees
 *        it automatically
 *
 * Returns the current value of name and sets name to NULL, so the cleanup
 * that ccol_scoped_ptr or ccol_scoped_ptr_mp registered does nothing at the
 * end of the scope. Use it to hand the pointer over to code outside the
 * enclosing scope (as a function's return value, for example), so that the
 * pointer isn't freed in that scope.
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
 *   return ccol_scoped_ptr_release(buf); // the caller owns it from here
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
 * @brief Check if type is an integral or floating-point type (internal helper)
 *
 * Uses C11 _Generic to find out whether a value has a standard numeric
 * type: a signed integer, an unsigned integer or a floating-point type,
 * with or without const.
 *
 * @param x The value to check
 * @return true for a numeric type, and false for every other type
 *
 * @note It handles char, short, int, long and long long, signed and unsigned
 * @note It handles float, double and long double
 * @note It handles the const form of every type above
 */
#if defined(_CCOL_CLANG_GENERIC_ASSOC_WARNING)
#define _ccol_is_integral_type(x)                                           \
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
#define _ccol_is_integral_type(x)     \
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
 * (internal helper)
 *
 * Uses C11 _Generic to find out whether a pointer points to a standard
 * numeric type.
 *
 * @param x The pointer to check
 * @return true for a pointer to a numeric type, and false for every other
 *         pointer
 *
 * @note It handles a pointer to every type that _ccol_is_integral_type()
 * checks
 */
#if defined(_CCOL_CLANG_GENERIC_ASSOC_WARNING)
#define _ccol_is_integral_ptr(x)                                            \
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
#define _ccol_is_integral_ptr(x)        \
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
 * Uses C11 _Generic to find out whether a pointer points to a signed
 * integer type; the BST map uses it to pick how it compares a key.
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
 * @brief Check if data is a char pointer (string) (internal helper)
 *
 * Uses C11 _Generic to find out whether data is a pointer to a char type,
 * signed char and unsigned char pointers included.
 *
 * @param data The value to check
 * @return true for a char pointer, and false for every other value
 *
 * @note The Clang form suppresses the warnings about unreachable code
 * @note unsigned char* counts as a string type
 * @note It also returns true for an array of a character type, because
 * _Generic turns an array into a pointer to its first element before it
 * picks an association. _ccol_is_char_array() relies on that and tells the
 * two apart
 */
/* Gives 1 when the TYPE of data is an array of a character type, and 0 when
 * it is a character pointer. _ccol_is_char_ptr() guards it, so only a
 * character array or a character pointer gets here through
 * _ccol_is_char_array().
 *
 * _ccol_char_ptr_flavour() names the character-pointer type that data
 * decays to. A pointer has that type already, so the two are compatible,
 * while an array has an array type, which is never compatible with a
 * pointer type. __builtin_types_compatible_p() ignores the top-level
 * qualifiers of both operands, so a pointer object that is itself const or
 * volatile, like `const char *const NAME`, counts as a pointer here. A test
 * on the type of &(data) couldn't give that answer, because &(data) carries
 * data's top-level qualifier into the pointed-to type, and a _Generic list
 * would have to name every qualified spelling of every character pointer.
 *
 * The expression is an integer constant expression that never evaluates
 * data and never needs data's address, so an rvalue is fine here. */
#define _ccol_type_is_array_of_char(data)          \
  (!__builtin_types_compatible_p(__typeof__(data), \
                                 __typeof__(_ccol_char_ptr_flavour(data))))

#if defined(_CCOL_CLANG_GENERIC_ASSOC_WARNING)
#define _ccol_is_char_ptr(data)                                             \
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
 * @brief Check if data is a char array (not pointer to pointer) (internal
 * helper)
 *
 * Tells a char array (char arr[]) apart from a char pointer (char*).
 *
 * @param data The value to check. Only its type is read, so an rvalue is as
 * fine as an lvalue.
 * @return true for a char array, and false for a pointer or another type
 *
 * @note A char pointer that is qualified itself, such as a
 * `static const char *const NAME` or a `char *volatile p`, is a pointer and
 * not an array: the answer depends only on whether data's type is an array
 * type.
 * @note It helps you pick how to store a string
 */
#define _ccol_is_char_array(data) \
  (_ccol_is_char_ptr((data)) && _ccol_type_is_array_of_char(data))
#else
#define _ccol_is_char_ptr(data)    \
  _Generic((data),                 \
      char *: true,                \
      const char *: true,          \
      signed char *: true,         \
      const signed char *: true,   \
      unsigned char *: true,       \
      const unsigned char *: true, \
      default: false)

#define _ccol_is_char_array(data) \
  (_ccol_is_char_ptr((data)) && _ccol_type_is_array_of_char(data))
#endif

/* Every enumerator below has an explicit value on purpose, even though C
 * would give the same values if they were implicit, because with implicit
 * values inserting a new enumerator anywhere but at the very end quietly
 * renumbers every later entry. It's the same hazard ccol_retval_t guards
 * against (see that enum's doc comment in this file). Add a new enumerator
 * at the very end with the next free number, so every existing enumerator
 * keeps its value. */
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
                           or value type. It isn't the same as ccol_char:
                           the platform defines whether a plain `char` is
                           signed, so `signed char` needs a type of its own
                           for the comparison to be really signed on every
                           platform. A `signed char *` resolves to
                           ccol_string. */
} ccol_data_type;

/**
 * @brief The byte size of the C type that a ccol_data_type names, or 0 when
 * the type has no single fixed width
 *
 * Returns sizeof() of the C type the enumerator stands for, for every
 * enumerator that names one specific C type. ccol_string, ccol_other_types
 * and any value it doesn't recognize give 0, since a key or value of those
 * types can rightly have any size and there's no single width to check a
 * caller's cmap_pair against.
 *
 * A caller can build a cmap_pair by hand and pass it through a map module's
 * raw layer, such as chmap_insert_elem() or cbmap_insert_elem(), and the map
 * modules use this function to validate such a pair. A container that
 * dispatches on its declared key type reads a fixed number of bytes out of
 * the key it gets, so it rejects a key_pair whose size disagrees with this
 * value with ccol_invalid_args. Without that check the container would read
 * past the caller's buffer, and it could also store a key that no correctly
 * typed lookup can ever find again. A caller building a cmap_pair by hand
 * can use this function to give the pair the right size.
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

#if defined(_CCOL_CLANG_GENERIC_ASSOC_WARNING)
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

/* __ccol_dcdt_type_probe is a fresh, zero-initialized object with the type
 * of (data), and the code below uses it instead of (data) itself. The
 * operand of __builtin_classify_type() is a pure compile-time
 * classification of a type, like the operand of sizeof or _Generic, and
 * nothing ever evaluates it for its VALUE. But Clang's -Wuninitialized
 * analysis doesn't treat it as an unevaluated context, although it does
 * treat sizeof and _Generic that way.
 *
 * A caller can write a (data) that dereferences a type-tracking companion
 * variable that is uninitialized on purpose, such as
 * hm_name##__ccol_key_type_var in chmap_declare(). That variable has to
 * stay uninitialized so the same macro also works as a struct member
 * declaration, where C doesn't allow an initializer at all. With such a
 * (data), using it directly triggers a false "used before initialized"
 * diagnostic.
 *
 * __ccol_dcdt_type_probe has exactly the type of (data), and since
 * __typeof__(data) is unevaluated as well, this doesn't depend on the value
 * of (data) either. The probe is a well-defined object, so nothing here
 * reads indeterminate memory. That gets rid of the false positive without
 * changing either what this macro does or the single-evaluation contract of
 * (data). The name is specific enough that a real caller's (data)
 * expression can't collide with it; ccol_scoped_ptr_release and
 * __ccol_released_ptr in this header follow the same precedent for the same
 * reason. */
#define ccol_determine_ccol_data_type(data)                           \
  ({                                                                  \
    ccol_data_type __ccol_dcdt_r = ccol_other_types;                  \
    if (_ccol_is_char_array((data))) {                                \
      __ccol_dcdt_r = ccol_string;                                    \
    } else if (_ccol_is_char_ptr((data))) {                           \
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
 * below can select on it. _ccol_is_char_ptr() can't do this job, since its
 * Clang form is a statement expression and that's never a constant
 * expression.
 *
 * The list is the same one _ccol_is_char_ptr() has, and the two must
 * stay in step: both describe the set of types the library treats as a
 * string (a run of bytes with its own length) rather than a scalar.
 *
 * @param lvalue An expression whose TYPE is inspected. It is never
 *               evaluated.
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
 * _ccol_declared_or_own_type() only reads the type of this expression.
 * Every association is a cast of a null pointer constant that this header
 * writes itself, so each one is well formed for any expr, and that matters
 * because __builtin_choose_expr() type-checks the arm it doesn't choose.
 *
 * The default association answers char * for an expr that isn't a
 * character pointer at all. No correct call gets there: a container whose
 * declared type is a character pointer, keyed by a caller with a struct for
 * example, hits the type error at the initializer that follows, which is
 * where a reader can do something about it.
 *
 * @param expr An expression whose TYPE is inspected. _Generic never
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
 * key or value type. expr is the expression the caller wrote.
 *
 * When the declared type is NOT a character pointer this gives the declared
 * type, so the copy is a plain C assignment that converts expr's value by
 * the compiler's conversion rules. That's what stops a same-width
 * expression of another type from reaching the container as a raw bit
 * pattern, and an expression with no implicit conversion to the declared
 * type is a compile error right at the copy.
 *
 * When the declared type IS a character pointer this gives the type of expr
 * itself. To this library every character-pointer type is one single key
 * type and one single value type: char *, signed char * and unsigned char *
 * all classify as ccol_string, a NUL-terminated string, and the container
 * compares the string's bytes and never the pointer type. A map keyed on
 * unsigned char * therefore has to stay reachable through a char * that
 * addresses the same string, and converting to the declared type would make
 * that a -Wpointer-sign error and take the feature away. Either way no bit
 * pattern can be reinterpreted here, because all these types end up in the
 * same string branch of _populate_cmap_pair().
 *
 * The type of the character-pointer arm comes from _ccol_char_ptr_flavour()
 * and not from __typeof__(expr), because a caller writing a char buf[64]
 * key, or a string literal, would otherwise give that arm an ARRAY type, and
 * an array can't be assigned from another array. The decayed pointer is
 * also what _populate_cmap_pair() wants: its _ccol_is_char_ptr() branch
 * reads the pointer and measures the string with strlen + 1, which is
 * exactly the pair its array branch makes.
 *
 * __builtin_choose_expr() type-checks BOTH arms while only the chosen arm
 * decides the type, so every arm must be a well-formed expression for any
 * type a caller can bring here. A plain "(expr) + 0" isn't: it fails for a
 * struct value type, and for a pointer to an incomplete type such as the
 * cvec value of a map of vectors. _Generic answers the same question with
 * associations this header writes itself, so each one is well formed
 * whatever expr turns out to be.
 *
 * _ccol_unqual_type() drops the top-level qualifiers of the choice, so a map
 * declared with a const key type gets a modifiable copy that
 * _ccol_clear_padding() can write to.
 *
 * expr is never evaluated: __typeof__() doesn't evaluate its operand and
 * __builtin_choose_expr() evaluates neither arm. So a caller expression with
 * a side effect is evaluated exactly once, by the initializer that follows
 * this type.
 *
 * @param type_var The companion type variable of the container
 * @param expr     The expression that the caller wrote
 */
#define _ccol_declared_or_own_type(type_var, expr)                             \
  _ccol_unqual_type(__builtin_choose_expr(_ccol_type_is_char_ptr(*(type_var)), \
                                          _ccol_char_ptr_flavour(expr),        \
                                          *(type_var)))

/**
 * @brief 1 when the type T is an array type, and 0 for every other object
 *        type.
 *
 * The comma operator applies the usual conversions to *(T *)0, so an array
 * type decays to a pointer type and any other type keeps its type minus
 * the top-level qualifiers. __builtin_types_compatible_p() ignores
 * top-level qualifiers, so the two types are compatible exactly when T
 * isn't an array type. A function type decays too and also gives 1, which
 * is harmless here since no container can store a function either.
 *
 * The result is an integer constant expression and nothing is evaluated.
 *
 * @param T A type name, or an expression whose type is inspected
 */
#define _ccol_type_is_array(T)                  \
  (!__builtin_types_compatible_p(__typeof__(T), \
                                 __typeof__((void)0, *(__typeof__(T) *)0)))

/**
 * @brief Refuse, at compile time, an array key type or an array value type.
 *
 * The type-inferred macros of the maps and of the LRU cache copy a key or a
 * value through a temporary of the declared type, and C has no array
 * assignment, so an array type would reach the container as a pointer to
 * the caller's array and the container would store the address instead of
 * the elements. These two declarations make such a type a compile error,
 * with a message telling the caller to wrap the array in a struct, which the
 * containers copy, hash and compare by value.
 *
 * It expands to two static assertions, each of them a declaration, so it is
 * valid anywhere the declarations of the companion type variables are
 * valid, inside a struct too. It ends without a semicolon, like a
 * declaration the caller completes.
 *
 * @param container A string literal that names the macro family
 * @param key_t     The declared key type
 * @param val_t     The declared value type
 */
#define _ccol_refuse_array_types(container, key_t, val_t)            \
  _Static_assert(!_ccol_type_is_array(key_t), container              \
                 ": an array key type is not supported; wrap the "   \
                 "array in a struct");                               \
  _Static_assert(!_ccol_type_is_array(val_t), container              \
                 ": an array value type is not supported; wrap the " \
                 "array in a struct")

/* The value of x, an expression of a non-array type, as an rvalue whose
 * type has no top-level qualifier. Only its type is ever used.
 *
 * Clang applies the comma operator's lvalue conversion to the type that
 * __typeof__() reports, so `(void)0, x` drops const and volatile there. GCC
 * before version 11 keeps the qualifiers in that position, but a cast to a
 * qualified type gives the unqualified type on every GCC version. For a
 * struct or a union the cast is the GNU extension that casts a value to its
 * own type, which __extension__ keeps quiet under -Wpedantic. Clang refuses
 * that extension, and that's why each compiler gets its own spelling. */
#if defined(__clang__)
#define _ccol_unqual_nonarray_rvalue(x) ((void)0, (x))
#else
#define _ccol_unqual_nonarray_rvalue(x) (__extension__(__typeof__(x))(x))
#endif

/**
 * @brief The type of x without its top-level qualifiers, for a temporary that
 *        a macro writes to.
 *
 * A value macro that copies into its own temporary, or clears that
 * temporary's padding, needs a modifiable object even when the declared
 * type is const, like `const int` or `const char *const`, and this gives
 * that type. An array type keeps its own type, because the conversion that
 * drops the qualifiers would also turn an array into a pointer of a
 * different size; the qualifiers of an array type belong to its elements
 * and stay with them.
 *
 * The arm that isn't chosen sees the constant 0 in place of x, so a cast is
 * never applied to an array. Neither __typeof__() nor
 * __builtin_choose_expr() evaluates its operand, so x is never evaluated.
 *
 * @param x An expression whose TYPE is inspected
 */
#define _ccol_unqual_type(x)        \
  __typeof__(__builtin_choose_expr( \
      _ccol_type_is_array(x), (x),  \
      _ccol_unqual_nonarray_rvalue( \
          __builtin_choose_expr(_ccol_type_is_array(x), 0, (x)))))

/**
 * @brief Set every padding byte of *obj_ptr to zero, where the compiler can.
 *
 * A container that hashes or compares a key of a type it doesn't know, a
 * struct for example, reads every byte of the key, padding included. C
 * leaves a struct's padding unspecified after a store of the struct value,
 * and an optimizing compiler really does copy such a value member by member,
 * so a copy of a key whose padding the caller zeroed can still have stack
 * garbage in its padding. The container macros call this on their own copy
 * of a key, so two keys with equal members give equal bytes.
 *
 * __builtin_clear_padding() does exactly that where the compiler has it.
 * Elsewhere the macro does nothing, and the copy keeps whatever bytes the
 * compiler copied from the caller's key. For a type without padding the
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
 * Works out the right way to fill a cmap_pair from the type of the data,
 * with special handling for a string (a char array or a char pointer) so
 * that the size counts the null terminator.
 *
 * @param pair A pointer to the cmap_pair to populate
 * @param data The data to store (a value, an array, or a pointer)
 *
 * @note For a char array it stores a pointer to the array, and the size
 * counts the null terminator
 * @note For a char pointer it dereferences to get the string, and the size
 * counts the null terminator; a NULL char pointer gives ptr == NULL and a
 * size of 0, which every raw entry point refuses as ccol_invalid_args
 * @note For every other type it stores a pointer to the data, and the size is
 * sizeof(data)
 * @note A char pointer is read as a value through a choice the compiler
 * makes from data's type, so no other type is ever read as a pointer
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
#define _populate_cmap_pair(pair, data)                                      \
  do {                                                                       \
    if (_ccol_is_char_array((data))) {                                       \
      (pair)->ptr = (char *)&(data);                                         \
      (pair)->size = strlen((char *)(pair)->ptr) + 1;                        \
    } else if (_ccol_is_char_ptr((data))) {                                  \
      /* This branch gets compiled for every type of data, so the choice     \
       * below gives data itself only when its type is a character pointer,  \
       * and a null pointer for every other type. That way no object is ever \
       * read through a pointer type wider than itself, and we need no       \
       * diagnostic pragma, which GCC 10 can't take inside the argument of   \
       * another macro. The round trip through uintptr_t drops the const of  \
       * the pointed-to characters without a cast that -Wcast-qual reports;  \
       * this layer only reads through the pointer. */                       \
      (pair)->ptr = (void *)(uintptr_t)__builtin_choose_expr(                \
          _ccol_type_is_char_ptr(data), (data), (const char *)0);            \
      /* A NULL pointer is not a string. It gives a pair of size 0, which    \
       * the raw layer refuses as ccol_invalid_args, and strlen never        \
       * reads through it. */                                                \
      (pair)->size =                                                         \
          (pair)->ptr ? strlen((char *)(pair)->ptr) + 1 : (size_t)0;         \
    } else {                                                                 \
      /* cmap_pair.ptr is void *, but every entry point that reads a pair    \
       * takes it as const cmap_pair * and only reads the bytes. The cast    \
       * is what lets a caller key a map from a const array; without it      \
       * -Wdiscarded-qualifiers makes that a hard error in a -Werror         \
       * build. This layer never writes through that const.                  \
       */                                                                    \
      (pair)->ptr = (void *)&(data);                                         \
      (pair)->size = sizeof((data));                                         \
    }                                                                        \
  } while (0)

/* ========================================================================== */
/*                         UTILITY MACROS                                     */
/* ========================================================================== */

/**
 * @brief Return minimum of two values. Internal; the library and the inline
 *        code of its headers use it.
 *
 * Evaluates both arguments and returns the smaller value.
 *
 * @param a The first value
 * @param b The second value
 * @return The smaller of a and b
 *
 * @warning An argument can be evaluated more than once
 */
#define _ccol_min(a, b) ((a) < (b) ? (a) : (b))

/**
 * @brief Return maximum of two values. Internal; the library and the inline
 *        code of its headers use it.
 *
 * Evaluates both arguments and returns the larger value.
 *
 * @param a The first value
 * @param b The second value
 * @return The larger of a and b
 *
 * @warning An argument can be evaluated more than once
 */
#define _ccol_max(a, b) ((a) > (b) ? (a) : (b))

/**
 * @brief Type-inferred comparison macro (internal helper)
 *
 * Compares the two values of type T that ptr1 and ptr2 address and returns
 * the usual comparison result.
 *
 * @param ptr1 A pointer to the first value
 * @param ptr2 A pointer to the second value
 * @param T The type of the two values
 * @return A negative value if *ptr1 < *ptr2, 0 if they are equal, and a
 *         positive value if *ptr1 > *ptr2
 *
 * @note It uses the three-way comparison (a > b) - (a < b)
 * @note For an integer type the result is -1, 0 or 1
 * @note ptr1 and ptr2 need no alignment for T, and the object each one
 *       addresses may have any effective type; see the note below.
 */
/* Both operands are read with memcpy rather than by dereferencing a pointer
 * cast to T *. Two separate requirements make that cast wrong, and meeting
 * one of them says nothing about the other.
 *
 * The first one is alignment. A caller gets here through cbstmap's raw
 * cmap_pair layer (cbmap_insert_elem, cbmap_get_elem_ref and
 * cbmap_delete_elem), which accepts a pair the caller built by hand with no
 * alignment guarantee at all: key bytes parsed out of a packed network frame
 * sit at whatever offset the frame put them. A load through a T * from such
 * an address is undefined. It faults on a strict-alignment target, and
 * UndefinedBehaviorSanitizer reports it on every target ("load of
 * misaligned address ... which requires N byte alignment").
 *
 * The second is the effective type. C11 6.5p7 allows a load through an
 * lvalue of type T only when the stored object really has type T, and the
 * bytes behind a cmap_pair are type-erased by construction, so the cast
 * breaks that rule at ANY address, aligned or not. Nothing traps on it, the
 * optimizer just takes a licence it isn't entitled to.
 *
 * A memcpy with a compile-time constant size takes care of both, and the
 * compiler emits the same single load the cast would, so it costs nothing.
 * chashmap reads every key the same way, and the comment on hash_key_data in
 * chashmap.c gives the same two reasons. */
#define _ccol_typed_cmp(ptr1, ptr2, T)                                         \
  ({                                                                           \
    T __ccol_tcmp_lhs;                                                         \
    T __ccol_tcmp_rhs;                                                         \
    memcpy(&__ccol_tcmp_lhs, (ptr1), sizeof(T));                               \
    memcpy(&__ccol_tcmp_rhs, (ptr2), sizeof(T));                               \
    (__ccol_tcmp_lhs > __ccol_tcmp_rhs) - (__ccol_tcmp_lhs < __ccol_tcmp_rhs); \
  })

#pragma GCC visibility pop
