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

#include <common.h>
#include <internal/cgrowbuf.h>
#include <internal/cpow2.h>
#include <internal/cprocsintern.h>
#include <internal/test_helpers/common.h>
#include <internal/test_helpers/cprocsintern.h>
#include <limits.h>
#include <stdatomic.h>
#include <time.h>

/* ThreadSanitizer doesn't model a lock that the child of fork() initializes
 * again. It keeps the write lock held by the thread that forked, so it sees
 * no release that a later lock in the child could follow, and it reports a
 * data race between two accesses that both hold the lock, or an inversion
 * against the lock it believes is held. So the child tells it the write lock
 * is released, using the pair of annotations that surround an unlock with no
 * unlock between them, and the lock that the init then makes is a new lock in
 * the model as well. */
#if defined(__SANITIZE_THREAD__)
#define _CCOL_COMMON_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define _CCOL_COMMON_TSAN 1
#endif
#endif
#if defined(_CCOL_COMMON_TSAN)
int __tsan_mutex_pre_unlock(void *addr, unsigned flags);
void __tsan_mutex_post_unlock(void *addr, unsigned flags);
#endif

int _ccol_rw_lock_reinit_in_child_impl(void *lock) {
  _ccol_rw_lock_t *rw = lock;
#if defined(_CCOL_COMMON_TSAN)
  (void)__tsan_mutex_pre_unlock(rw, 0);
  __tsan_mutex_post_unlock(rw, 0);
#endif
  memset(rw, 0, sizeof(*rw));
  return _ccol_rw_lock_init(*rw);
}

/* Returns the smallest power of two that is >= input, or ccol_invalid_size
 * when that power doesn't fit in a size_t. For an input of 2 or more the
 * answer is 2 to the power of the bit length of input - 1, and one count of
 * leading zeros gives that bit length, so the whole thing is a subtraction, a
 * count and a shift. The width of size_t is named only once, as the width of
 * the builtin's operand. That works because unsigned long is as wide as
 * size_t on every target we support (LP64 and ILP32), which the
 * _Static_assert holds us to. */
_Static_assert(sizeof(unsigned long) == sizeof(size_t),
               "the power-of-two rounding needs size_t as wide as long");
size_t _ccol_find_nearest_gte_power_of_two(size_t input) {
  if (input <= 1) return 1;
  unsigned long below = (unsigned long)(input - 1);
  int bits = (int)(sizeof(unsigned long) * CHAR_BIT) - __builtin_clzl(below);
  /* bits is the bit length of input - 1, somewhere from 1 to the width. When
   * it equals the width the answer would be 2^width, and a size_t can't hold
   * that. */
  if (bits >= (int)(sizeof(unsigned long) * CHAR_BIT)) return ccol_invalid_size;
  return (size_t)1 << bits;
}

/* ========================================================================== */
/*                         GROWABLE BYTE BUFFER                               */
/* ========================================================================== */

void ccol_growbuf_init(ccol_growbuf_t *b, ccol_memmgmt_procs_t *mp) {
  b->m_procs = mp;
  b->buf = _ccol_mem_alloc(mp, 256);
  b->len = 0;
  b->cap = b->buf ? 256 : 0;
  b->oom = b->buf ? false : true;
  if (b->buf) b->buf[0] = '\0';
}

void ccol_growbuf_init_hint(ccol_growbuf_t *b, ccol_memmgmt_procs_t *mp,
                            size_t hint) {
  b->m_procs = mp;
  b->len = 0;
  /* The hint asks for a store of hint content bytes plus the terminating NUL.
   * A hint of SIZE_MAX names a size that can't be represented, so we refuse
   * it here before hint + 1 is ever formed. That expression would wrap to 0,
   * the 64-byte floor would win, and the caller would get a buffer much
   * smaller than it asked for with success reported. */
  if (hint == SIZE_MAX) {
    b->buf = NULL;
    b->cap = 0;
    b->oom = true;
    return;
  }
  size_t cap = hint + 1 > 64 ? hint + 1 : 64;
  b->buf = _ccol_mem_alloc(mp, cap);
  b->cap = b->buf ? cap : 0;
  b->oom = b->buf ? false : true;
  if (b->buf) b->buf[0] = '\0';
}

/* Doubles the buffer's capacity until it holds 'needed' bytes, and sets
 * b->oom when the reallocation fails or a size_t overflows. */
static void growbuf_grow(ccol_growbuf_t *b, size_t needed) {
  if (b->oom) return;
  size_t new_cap =
      b->cap ? (b->cap > SIZE_MAX / 2 ? SIZE_MAX : b->cap * 2) : 256;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2) {
      b->oom = true;
      return;
    }
    new_cap *= 2;
  }
  char *p = _ccol_mem_realloc(b->m_procs, b->buf, new_cap);
  if (!p) {
    b->oom = true;
    return;
  }
  b->buf = p;
  b->cap = new_cap;
}

void ccol_growbuf_append_slow(ccol_growbuf_t *b, const char *data, size_t n) {
  if (b->oom) return;
  /* This append needs b->len + n + 1 bytes, and that sum must be tested for
   * representability BEFORE it is formed. If we form it first it can wrap,
   * the wrapped value compares below b->cap, nothing grows, and the memcpy
   * below then runs with the caller's huge original n against a buffer that
   * has room for none of it.
   *
   * The subtraction can't wrap, because b->len is always below b->cap and so
   * at most SIZE_MAX - 1. A request whose size can't be represented is
   * refused just like a failed allocation, by latching oom, so a caller that
   * only checks for an error at the end of a build pass sees it too. */
  if (n > SIZE_MAX - 1 - b->len) {
    b->oom = true;
    return;
  }
  size_t needed = b->len + n + 1;
  if (needed > b->cap) growbuf_grow(b, needed);
  if (b->oom) return;
  /* An empty append is already done, since b->buf[b->len] holds the NUL that
   * every other path leaves there. Returning here also keeps a (NULL, 0)
   * append away from memcpy, whose pointer arguments must be valid even for
   * a length of zero. Callers write such an append quite naturally, because
   * the wrapper for a NUL-terminated string accepts a NULL string. */
  if (n == 0) return;
  memcpy(b->buf + b->len, data, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

/* ========================================================================== */
/*                         ALLOCATOR PROCS INTERN TABLE                       */
/* ========================================================================== */

/* The state of one slot. A slot moves from EMPTY to WRITING once, when a
 * thread claims it, and from WRITING to READY once, when that thread has
 * written the copy, and it never goes back. A thread only claims the first
 * slot it sees EMPTY, after seeing every earlier slot READY, so the claimed
 * slots are always a prefix of the table and a lookup can stop at the first
 * EMPTY slot. */
enum {
  _CCOL_PROCS_SLOT_EMPTY = 0,
  _CCOL_PROCS_SLOT_WRITING = 1,
  _CCOL_PROCS_SLOT_READY = 2
};

static ccol_memmgmt_procs_t _ccol_procs_slots[CCOL_PROCS_INTERN_CAPACITY];
static atomic_uchar _ccol_procs_slot_state[CCOL_PROCS_INTERN_CAPACITY];
#ifdef RUNNING_UNIT_TESTS
_Atomic unsigned _ccol_procs_intern_claim_delay_us_for_tests;
_Atomic unsigned _ccol_procs_intern_claim_wait_sightings_for_tests;
_Atomic unsigned _ccol_procs_intern_writing_sightings_for_tests;
#endif

static inline bool _ccol_procs_same(const ccol_memmgmt_procs_t *a,
                                    const ccol_memmgmt_procs_t *b) {
  return a->malloc == b->malloc && a->free == b->free &&
         a->calloc == b->calloc && a->realloc == b->realloc;
}

bool ccol_procs_is_interned(const ccol_memmgmt_procs_t *mp) {
  /* We compare integers here, because a relational comparison of pointers
   * into different objects is undefined. */
  uintptr_t addr = (uintptr_t)mp;
  uintptr_t base = (uintptr_t)&_ccol_procs_slots[0];
  return addr >= base && addr < base + sizeof(_ccol_procs_slots) &&
         (addr - base) % sizeof(ccol_memmgmt_procs_t) == 0;
}

/* Waits until slot i, which another thread has claimed, is READY. All the
 * claimer has left is a 32-byte copy and one store, so the wait is short. It
 * spins for a while and then sleeps a microsecond at a time, so a claimer
 * that the scheduler stopped (or a tool that runs one thread at a time) gets
 * the CPU back. A lookup must wait here rather than skip the slot, because
 * the slot may hold exactly the content the lookup is after. A lookup that
 * went past it would claim a second slot for that content, two callers would
 * then get two pointers for one content, and a burst of first uses could
 * fill the table with copies of it. */
static __attribute__((noinline, cold)) void _ccol_procs_wait_ready(size_t i) {
  unsigned spins = 0;
  while (atomic_load_explicit(&_ccol_procs_slot_state[i],
                              memory_order_acquire) != _CCOL_PROCS_SLOT_READY) {
    if (spins < 64) {
      spins++;
    } else {
      struct timespec ts = {0, 1000L};
      nanosleep(&ts, NULL);
    }
  }
}

ccol_retval_t ccol_procs_intern(ccol_memmgmt_procs_t *mp,
                                ccol_memmgmt_procs_t **out) {
  *out = NULL;
  if (mp == NULL) return ccol_success;
  /* A pointer into the table is interned already. */
  if (ccol_procs_is_interned(mp)) {
    *out = mp;
    return ccol_success;
  }
  /* Read the caller's struct once into a local, so every comparison and the
   * copy see the same four values. */
  ccol_memmgmt_procs_t want;
  memcpy(&want, mp, sizeof(want));
  for (size_t i = 0; i < CCOL_PROCS_INTERN_CAPACITY; i++) {
    unsigned char st =
        atomic_load_explicit(&_ccol_procs_slot_state[i], memory_order_acquire);
    if (st == _CCOL_PROCS_SLOT_EMPTY) {
      unsigned char expected = _CCOL_PROCS_SLOT_EMPTY;
      if (atomic_compare_exchange_strong_explicit(
              &_ccol_procs_slot_state[i], &expected, _CCOL_PROCS_SLOT_WRITING,
              memory_order_acq_rel, memory_order_acquire)) {
#ifdef RUNNING_UNIT_TESTS
        unsigned delay_us = atomic_load_explicit(
            &_ccol_procs_intern_claim_delay_us_for_tests, memory_order_relaxed);
        unsigned sightings = atomic_load_explicit(
            &_ccol_procs_intern_claim_wait_sightings_for_tests,
            memory_order_relaxed);
        if (delay_us && sightings) {
          /* The delay is then only the bound of a wait for that many
           * sightings of a slot being written. */
          for (unsigned waited = 0;
               waited < delay_us &&
               atomic_load(&_ccol_procs_intern_writing_sightings_for_tests) <
                   sightings;
               waited += 100) {
            struct timespec ts = {0, 100000L};
            nanosleep(&ts, NULL);
          }
        } else if (delay_us) {
          struct timespec ts = {(time_t)(delay_us / 1000000u),
                                (long)(delay_us % 1000000u) * 1000L};
          nanosleep(&ts, NULL);
        }
#endif
        _ccol_procs_slots[i] = want;
        atomic_store_explicit(&_ccol_procs_slot_state[i],
                              _CCOL_PROCS_SLOT_READY, memory_order_release);
        *out = &_ccol_procs_slots[i];
        return ccol_success;
      }
      /* Another thread claimed this slot first, so it is WRITING or READY. */
      st = expected;
    }
    /* Each content sits in at most one slot, and every slot before the one a
     * thread claims is READY at the moment it claims it, so a slot that
     * another thread is still writing is read before the lookup moves
     * on. */
#ifdef RUNNING_UNIT_TESTS
    if (st == _CCOL_PROCS_SLOT_WRITING)
      atomic_fetch_add(&_ccol_procs_intern_writing_sightings_for_tests, 1);
#endif
    if (st == _CCOL_PROCS_SLOT_WRITING) _ccol_procs_wait_ready(i);
    if (_ccol_procs_same(&_ccol_procs_slots[i], &want)) {
      *out = &_ccol_procs_slots[i];
      return ccol_success;
    }
  }
  return ccol_container_full;
}

#ifdef RUNNING_UNIT_TESTS
size_t _ccol_procs_intern_used_for_tests(void) {
  size_t n = 0;
  while (n < CCOL_PROCS_INTERN_CAPACITY &&
         atomic_load_explicit(&_ccol_procs_slot_state[n],
                              memory_order_acquire) != _CCOL_PROCS_SLOT_EMPTY)
    n++;
  return n;
}

void _ccol_procs_intern_truncate_for_tests(size_t keep) {
  for (size_t i = keep; i < CCOL_PROCS_INTERN_CAPACITY; i++) {
    memset(&_ccol_procs_slots[i], 0, sizeof(_ccol_procs_slots[i]));
    atomic_store_explicit(&_ccol_procs_slot_state[i], _CCOL_PROCS_SLOT_EMPTY,
                          memory_order_release);
  }
}
#endif /* RUNNING_UNIT_TESTS */

#ifdef RUNNING_UNIT_TESTS
/* See _ccol_atfork_module_t in common.h for what this is meant to catch. */
static const char *const _ccol_atfork_module_names[_ccol_atfork_module_count] =
    {"clogger", "cthreadcomm", "cthreadpool", "chttpserver", "chttpclient"};

/* The handlers that already ran in the fork the process is preparing, in the
   order they ran. _ccol_atfork_order_reset empties it after every fork, in
   the parent and in the child. */
static unsigned char _ccol_atfork_seq[_ccol_atfork_module_count];
static unsigned _ccol_atfork_seq_len;

/* _ccol_atfork_ran_before[a][b] records that a's handler ran before b's
   handler in an earlier fork. */
static bool _ccol_atfork_ran_before[_ccol_atfork_module_count]
                                   [_ccol_atfork_module_count];

#if defined(_CCOL_CLOEXEC_GATE)
extern bool _ccol_cloexec_gate_prepared_for_tests;
#endif

void _ccol_atfork_order_record(_ccol_atfork_module_t ccol_module) {
  if ((unsigned)ccol_module >= (unsigned)_ccol_atfork_module_count) return;
#if defined(_CCOL_CLOEXEC_GATE)
  /* The close-on-exec gate has to be the last prepare handler; see
     _ccol_cloexec_gate_register. */
  if (_ccol_cloexec_gate_prepared_for_tests)
    _ccol_fatal_err(
        "fork-prepare handler of %s ran after the close-on-exec gate, so a "
        "fork() can wait for a lock whose holder waits for the gate",
        _ccol_atfork_module_names[ccol_module]);
#endif

  /* There's no lock here on purpose. This only runs inside a fork-prepare
     handler, and those handlers run one sequence at a time under the C
     library's atfork lock, so there is no concurrent caller to keep out. A
     lock here would nest one more lock inside the very handlers whose nesting
     this code is policing, which is the last thing we want to add to them. */
  /* A handler registered once runs once per fork, so it can only be in the
     sequence already when it was registered twice. Its pairs with the
     handlers before it are recorded by then, and there's nothing more to
     learn. */
  for (unsigned i = 0; i < _ccol_atfork_seq_len; i++) {
    if (_ccol_atfork_seq[i] == (unsigned char)ccol_module) return;
  }

  for (unsigned i = 0; i < _ccol_atfork_seq_len; i++) {
    _ccol_atfork_module_t earlier = (_ccol_atfork_module_t)_ccol_atfork_seq[i];
    if (_ccol_atfork_ran_before[ccol_module][earlier]) {
      _ccol_fatal_err(
          "fork-prepare handler order inverted: %s ran before %s in an earlier "
          "fork and after it in this one, so the locks the two handlers hold "
          "nest in opposite orders between two forks",
          _ccol_atfork_module_names[ccol_module],
          _ccol_atfork_module_names[earlier]);
    }
    _ccol_atfork_ran_before[earlier][ccol_module] = true;
  }

  if (_ccol_atfork_seq_len < (unsigned)_ccol_atfork_module_count)
    _ccol_atfork_seq[_ccol_atfork_seq_len++] = (unsigned char)ccol_module;
}

void _ccol_atfork_order_reset(void) { _ccol_atfork_seq_len = 0; }

/* Ends the sequence of every fork explicitly. The parent and child handlers
   of pthread_atfork run after every prepare handler of the fork, so the next
   fork always starts from an empty sequence. A reset that guessed the start
   of a fork from a handler running again would lose every pair between that
   handler and one that joins the order later, and would record false pairs
   for it. We register from a constructor, so this comes before every fork a
   test makes and every handler a module registers. */
__attribute__((constructor)) static void _ccol_atfork_order_register(void) {
  if (_ccol_at_fork(NULL, _ccol_atfork_order_reset, _ccol_atfork_order_reset) !=
      0) {
    _ccol_fatal_err("cannot register the fork-prepare order recorder");
  }
}
#endif /* RUNNING_UNIT_TESTS */

#if defined(_CCOL_CLOEXEC_GATE)
/* The close-on-exec gate; see its comment in common.h. A thread creating a
 * descriptor in two steps holds the read side across both of them and the
 * fork-prepare handler takes the write side, so a fork() waits until every
 * such descriptor is close-on-exec. The read side never waits for anything
 * except a fork, and a thread holding it takes no other lock. */
static _ccol_rw_lock_t _ccol_cloexec_gate;
static _ccol_once_flag_t _ccol_cloexec_gate_once = _CCOL_ONCE_INIT;
static _ccol_once_flag_t _ccol_cloexec_gate_reg_once = _CCOL_ONCE_INIT;
static bool _ccol_cloexec_gate_ok;

#ifdef RUNNING_UNIT_TESTS
/* How many times a thread entered and left the gate, so a test can check
 * that every creation site uses it and leaves it on every path. */
atomic_ulong _ccol_cloexec_gate_enters_for_tests;
atomic_ulong _ccol_cloexec_gate_leaves_for_tests;
/* True from the gate's prepare handler until the parent or the child
 * handler. Each module's fork-prepare handler reports itself to
 * _ccol_atfork_order_record, which ends the process if the gate already ran
 * in the same fork. Only prepare handlers run in that window, so the flag
 * doesn't need a lock. */
bool _ccol_cloexec_gate_prepared_for_tests;
/* How many times the gate's prepare handler started, counted before it waits
 * for the write side, so a test can tell that a fork() of another thread has
 * reached the gate and waits there. */
atomic_ulong _ccol_cloexec_gate_prepares_for_tests;
#endif

static void _ccol_cloexec_gate_init(void) {
  _ccol_cloexec_gate_ok = _ccol_rw_lock_init(_ccol_cloexec_gate) == 0;
}

void _ccol_cloexec_gate_enter(void) {
  _ccol_call_once(_ccol_cloexec_gate_once, _ccol_cloexec_gate_init);
  if (_ccol_cloexec_gate_ok) _ccol_rw_lock_rdlock(_ccol_cloexec_gate);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_ccol_cloexec_gate_enters_for_tests, 1,
                            memory_order_relaxed);
#endif
}

void _ccol_cloexec_gate_leave(void) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_ccol_cloexec_gate_leaves_for_tests, 1,
                            memory_order_relaxed);
#endif
  if (_ccol_cloexec_gate_ok) _ccol_rw_lock_unlock(_ccol_cloexec_gate);
}

static void _ccol_cloexec_gate_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&_ccol_cloexec_gate_prepares_for_tests, 1);
#endif
  _ccol_call_once(_ccol_cloexec_gate_once, _ccol_cloexec_gate_init);
  if (_ccol_cloexec_gate_ok) _ccol_rw_lock_wrlock(_ccol_cloexec_gate);
#ifdef RUNNING_UNIT_TESTS
  _ccol_cloexec_gate_prepared_for_tests = true;
#endif
}

static void _ccol_cloexec_gate_parent(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_cloexec_gate_prepared_for_tests = false;
#endif
  if (_ccol_cloexec_gate_ok) _ccol_rw_lock_unlock(_ccol_cloexec_gate);
}

/* The child has one thread, and a plain unlock doesn't always release a lock
 * that another thread took (see _ccol_rw_lock_reinit_in_child). */
static void _ccol_cloexec_gate_child(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_cloexec_gate_prepared_for_tests = false;
#endif
  if (_ccol_cloexec_gate_ok)
    _ccol_cloexec_gate_ok =
        _ccol_rw_lock_reinit_in_child(_ccol_cloexec_gate) == 0;
}

/* The prepare handlers of fork() run in the reverse order of registration,
 * so whatever registers first runs last. The gate has to run after every
 * prepare handler of the library, because those handlers take locks that a
 * thread can be holding while it waits to enter the gate. In the other order
 * a handler waits for that thread, the thread waits for the gate, and fork()
 * never returns. That's why _ccol_at_fork calls this before it registers any
 * handler, and the constructor below calls it at load, so no order of
 * constructors, of static linking or of first use can put a library handler
 * ahead of the gate. If the registration fails the gate still works, only
 * fork() won't wait for it. A new fork handler of the library should go
 * through _ccol_at_fork for this reason, and not straight to
 * pthread_atfork. */
static void _ccol_cloexec_gate_do_register(void) {
  (void)_ccol_at_fork_raw(_ccol_cloexec_gate_prepare, _ccol_cloexec_gate_parent,
                          _ccol_cloexec_gate_child);
}

void _ccol_cloexec_gate_register(void) {
  _ccol_call_once(_ccol_cloexec_gate_reg_once, _ccol_cloexec_gate_do_register);
}

__attribute__((constructor)) static void _ccol_cloexec_gate_at_load(void) {
  _ccol_cloexec_gate_register();
}
#endif /* _CCOL_CLOEXEC_GATE */
