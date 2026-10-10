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
#include <limits.h>
#include <stdatomic.h>

/* ThreadSanitizer does not model a lock that the child of fork() initializes
 * again: it keeps the write lock held by the thread that forked, so it sees
 * no release that a later lock in the child can follow, and it reports a data
 * race between two accesses that both hold the lock, or an inversion against
 * the lock that it thinks is held. The child therefore tells it that the
 * write lock is released, with the pair of annotations that surround an
 * unlock and no unlock between them, and the lock that the init then makes is
 * a new lock in the model too. */
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

int _ccol_rw_lock_reinit_in_child(void *lock) {
  ccol_rw_lock_t *rw = lock;
#if defined(_CCOL_COMMON_TSAN)
  (void)__tsan_mutex_pre_unlock(rw, 0);
  __tsan_mutex_post_unlock(rw, 0);
#endif
  memset(rw, 0, sizeof(*rw));
  return ccol_rw_lock_init(*rw);
}

/* Returns the smallest power of two that is >= input, or ccol_invalid_size
 * when that power does not fit in a size_t. For an input of at least 2, the
 * answer is 2 to the power of the bit length of input - 1, and one count of
 * leading zeros gives that bit length, so the whole computation is a
 * subtraction, a count and a shift. The width of size_t is named once, as
 * the width of the operand of the builtin: unsigned long has the width of
 * size_t on every target this library supports (LP64 and ILP32), and the
 * _Static_assert holds that. */
_Static_assert(sizeof(unsigned long) == sizeof(size_t),
               "the power-of-two rounding needs size_t as wide as long");
size_t _ccol_find_nearest_gte_power_of_two(size_t input) {
  if (input <= 1) return 1;
  unsigned long below = (unsigned long)(input - 1);
  int bits = (int)(sizeof(unsigned long) * CHAR_BIT) - __builtin_clzl(below);
  /* bits is the bit length of input - 1, from 1 up to the width. A bit
   * length equal to the width means that the answer is 2^width, which a
   * size_t cannot hold. */
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
  /* The store that the hint asks for is hint content bytes plus the NUL at
   * the end. A hint of SIZE_MAX names a size that is not representable, so
   * the function refuses it here instead of letting the code form hint + 1:
   * that expression wraps to 0, the 64-byte floor then wins, and the
   * function gives back a buffer much smaller than the caller asked for
   * while reporting success. */
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

/* Doubles the capacity of the buffer until it holds 'needed' bytes, and sets
 * b->oom if the reallocation fails or if a size_t overflows. */
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

void ccol_growbuf_append(ccol_growbuf_t *b, const char *data, size_t n) {
  if (b->oom) return;
  /* The size that this append needs is b->len + n + 1, and the code must test
   * that sum for representability BEFORE it forms the sum, not after. If the
   * code forms it first, the sum wraps, the wrapped value compares below
   * b->cap, no growth happens, and the memcpy below then runs with the
   * enormous original n of the caller against a buffer that has room for
   * none of it.
   *
   * A subtraction cannot wrap, because b->len is always below b->cap, and so
   * is at most SIZE_MAX - 1. The function refuses a request whose size is not
   * representable in the same way as a failed allocation: it latches oom, so
   * a caller that checks for an error only at the end of a build pass sees
   * the failure too. */
  if (n > SIZE_MAX - 1 - b->len) {
    b->oom = true;
    return;
  }
  size_t needed = b->len + n + 1;
  if (needed > b->cap) growbuf_grow(b, needed);
  if (b->oom) return;
  /* An empty append is already complete, because b->buf[b->len] holds the NUL
   * that every other path leaves. Returning here also stops a (NULL, 0)
   * append from reaching memcpy, whose pointer arguments must be valid even
   * for a length of zero; a caller writes such an append naturally, because
   * the wrapper for a NUL-terminated string accepts a NULL string. */
  if (n == 0) return;
  memcpy(b->buf + b->len, data, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

/* ========================================================================== */
/*                         ALLOCATOR PROCS INTERN TABLE                       */
/* ========================================================================== */

/* The state of one slot. A slot goes from EMPTY to WRITING once, when a
 * thread claims it, and from WRITING to READY once, when that thread has
 * written the copy, and it never goes back. A thread claims only the first
 * slot that it sees EMPTY, after it saw every earlier slot claimed, so the
 * claimed slots always form a prefix of the table and a lookup stops at the
 * first EMPTY slot. */
enum {
  _CCOL_PROCS_SLOT_EMPTY = 0,
  _CCOL_PROCS_SLOT_WRITING = 1,
  _CCOL_PROCS_SLOT_READY = 2
};

static ccol_memmgmt_procs_t _ccol_procs_slots[CCOL_PROCS_INTERN_CAPACITY];
static atomic_uchar _ccol_procs_slot_state[CCOL_PROCS_INTERN_CAPACITY];

static inline bool _ccol_procs_same(const ccol_memmgmt_procs_t *a,
                                    const ccol_memmgmt_procs_t *b) {
  return a->malloc == b->malloc && a->free == b->free &&
         a->calloc == b->calloc && a->realloc == b->realloc;
}

bool ccol_procs_is_interned(const ccol_memmgmt_procs_t *mp) {
  /* The comparison runs on integers, because a relational comparison of
   * pointers into different objects is undefined. */
  uintptr_t addr = (uintptr_t)mp;
  uintptr_t base = (uintptr_t)&_ccol_procs_slots[0];
  return addr >= base && addr < base + sizeof(_ccol_procs_slots) &&
         (addr - base) % sizeof(ccol_memmgmt_procs_t) == 0;
}

ccol_retval_t ccol_procs_intern(ccol_memmgmt_procs_t *mp,
                                ccol_memmgmt_procs_t **out) {
  *out = NULL;
  if (mp == NULL) return ccol_success;
  /* A pointer into the table is already interned. */
  if (ccol_procs_is_interned(mp)) {
    *out = mp;
    return ccol_success;
  }
  /* Read the caller's struct once, into a local, so that every comparison
   * and the copy use the same four values. */
  ccol_memmgmt_procs_t want;
  memcpy(&want, mp, sizeof(want));
  for (size_t i = 0; i < CCOL_PROCS_INTERN_CAPACITY; i++) {
    unsigned char st =
        atomic_load_explicit(&_ccol_procs_slot_state[i], memory_order_acquire);
    if (st == _CCOL_PROCS_SLOT_READY) {
      if (_ccol_procs_same(&_ccol_procs_slots[i], &want)) {
        *out = &_ccol_procs_slots[i];
        return ccol_success;
      }
      continue;
    }
    if (st == _CCOL_PROCS_SLOT_WRITING) continue;
    unsigned char expected = _CCOL_PROCS_SLOT_EMPTY;
    if (atomic_compare_exchange_strong_explicit(
            &_ccol_procs_slot_state[i], &expected, _CCOL_PROCS_SLOT_WRITING,
            memory_order_acq_rel, memory_order_acquire)) {
      _ccol_procs_slots[i] = want;
      atomic_store_explicit(&_ccol_procs_slot_state[i], _CCOL_PROCS_SLOT_READY,
                            memory_order_release);
      *out = &_ccol_procs_slots[i];
      return ccol_success;
    }
    /* Another thread claimed this slot first, so look at it again: when it is
     * already READY it can hold the content that this call wants. */
    if (expected == _CCOL_PROCS_SLOT_READY &&
        _ccol_procs_same(&_ccol_procs_slots[i], &want)) {
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
/* See ccol_atfork_module_t in common.h for what this exists to catch. */
static const char *const _ccol_atfork_module_names[ccol_atfork_module_count] = {
    "clogger", "cthreadcomm", "cthreadpool", "chttpserver", "chttpclient"};

/* The handlers that already ran in the fork that the process prepares now, in
   the order that they ran. _ccol_atfork_order_reset empties it after every
   fork, in the parent and in the child. */
static unsigned char _ccol_atfork_seq[ccol_atfork_module_count];
static unsigned _ccol_atfork_seq_len;

/* _ccol_atfork_ran_before[a][b] records that the handler of a ran before the
   handler of b in an earlier fork. */
static bool _ccol_atfork_ran_before[ccol_atfork_module_count]
                                   [ccol_atfork_module_count];

#if defined(_CCOL_CLOEXEC_GATE)
extern bool _ccol_cloexec_gate_prepared_for_tests;
#endif

void _ccol_atfork_order_record(ccol_atfork_module_t ccol_module) {
  if ((unsigned)ccol_module >= (unsigned)ccol_atfork_module_count) return;
#if defined(_CCOL_CLOEXEC_GATE)
  /* The close-on-exec gate must be the last prepare handler; see
     _ccol_cloexec_gate_register. */
  if (_ccol_cloexec_gate_prepared_for_tests)
    ccol_fatal_err(
        "fork-prepare handler of %s ran after the close-on-exec gate, so a "
        "fork() can wait for a lock whose holder waits for the gate",
        _ccol_atfork_module_names[ccol_module]);
#endif

  /* There is deliberately no lock here. This code runs only inside a
     fork-prepare handler, and those handlers run one sequence at a time,
     under the atfork lock of the C library, so there is no concurrent caller
     to exclude. A lock here would nest one more lock inside the handlers
     whose nesting this code exists to police, which is the last thing worth
     adding to them. */
  /* A handler that is registered once runs once in each fork, so it is in
     the sequence already only when it is registered twice. Its pairs with
     the handlers before it are recorded, and nothing more is learned. */
  for (unsigned i = 0; i < _ccol_atfork_seq_len; i++) {
    if (_ccol_atfork_seq[i] == (unsigned char)ccol_module) return;
  }

  for (unsigned i = 0; i < _ccol_atfork_seq_len; i++) {
    ccol_atfork_module_t earlier = (ccol_atfork_module_t)_ccol_atfork_seq[i];
    if (_ccol_atfork_ran_before[ccol_module][earlier]) {
      ccol_fatal_err(
          "fork-prepare handler order inverted: %s ran before %s in an earlier "
          "fork and after it in this one, so the locks the two handlers hold "
          "nest in opposite orders between two forks",
          _ccol_atfork_module_names[ccol_module],
          _ccol_atfork_module_names[earlier]);
    }
    _ccol_atfork_ran_before[earlier][ccol_module] = true;
  }

  if (_ccol_atfork_seq_len < (unsigned)ccol_atfork_module_count)
    _ccol_atfork_seq[_ccol_atfork_seq_len++] = (unsigned char)ccol_module;
}

void _ccol_atfork_order_reset(void) { _ccol_atfork_seq_len = 0; }

/* Ends the sequence of every fork explicitly. The parent and the child
   handlers of pthread_atfork run after every prepare handler of the fork, so
   the next fork always starts from an empty sequence. A reset that guessed
   the start of a fork from a handler that runs again would lose every pair
   between that handler and a handler that joins the order later, and would
   record false pairs for it. The registration runs from a constructor, so it
   precedes every fork that a test makes and every handler that a module
   registers. */
__attribute__((constructor)) static void _ccol_atfork_order_register(void) {
  if (ccol_at_fork(NULL, _ccol_atfork_order_reset, _ccol_atfork_order_reset) !=
      0) {
    ccol_fatal_err("cannot register the fork-prepare order recorder");
  }
}
#endif /* RUNNING_UNIT_TESTS */

#if defined(_CCOL_CLOEXEC_GATE)
/* The close-on-exec gate; see its comment in common.h. A thread that creates
 * a descriptor in two steps holds the read side across both steps, and the
 * fork-prepare handler takes the write side, so a fork() waits for every such
 * descriptor to be closed on exec. The read side never waits for anything
 * but a fork, and a thread that holds it takes no other lock. */
static ccol_rw_lock_t _ccol_cloexec_gate;
static ccol_once_flag_t _ccol_cloexec_gate_once = CCOL_ONCE_INIT;
static ccol_once_flag_t _ccol_cloexec_gate_reg_once = CCOL_ONCE_INIT;
static bool _ccol_cloexec_gate_ok;

#ifdef RUNNING_UNIT_TESTS
/* How many times a thread entered and left the gate, so that a test can
 * check that every creation site uses it and leaves it on every path. */
atomic_ulong _ccol_cloexec_gate_enters_for_tests;
atomic_ulong _ccol_cloexec_gate_leaves_for_tests;
/* True from the prepare handler of the gate until the parent or the child
 * handler. Each fork-prepare handler of a module reports itself to
 * _ccol_atfork_order_record, which ends the process when the gate has
 * already run in the same fork. Only prepare handlers run in that window, so
 * the flag needs no lock. */
bool _ccol_cloexec_gate_prepared_for_tests;
#endif

static void _ccol_cloexec_gate_init(void) {
  _ccol_cloexec_gate_ok = ccol_rw_lock_init(_ccol_cloexec_gate) == 0;
}

void _ccol_cloexec_gate_enter(void) {
  ccol_call_once(_ccol_cloexec_gate_once, _ccol_cloexec_gate_init);
  if (_ccol_cloexec_gate_ok) ccol_rw_lock_rdlock(_ccol_cloexec_gate);
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
  if (_ccol_cloexec_gate_ok) ccol_rw_lock_unlock(_ccol_cloexec_gate);
}

static void _ccol_cloexec_gate_prepare(void) {
  ccol_call_once(_ccol_cloexec_gate_once, _ccol_cloexec_gate_init);
  if (_ccol_cloexec_gate_ok) ccol_rw_lock_wrlock(_ccol_cloexec_gate);
#ifdef RUNNING_UNIT_TESTS
  _ccol_cloexec_gate_prepared_for_tests = true;
#endif
}

static void _ccol_cloexec_gate_parent(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_cloexec_gate_prepared_for_tests = false;
#endif
  if (_ccol_cloexec_gate_ok) ccol_rw_lock_unlock(_ccol_cloexec_gate);
}

/* The child has one thread, and a plain unlock does not always release a
 * lock that another thread took (see ccol_rw_lock_reinit_in_child). */
static void _ccol_cloexec_gate_child(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_cloexec_gate_prepared_for_tests = false;
#endif
  if (_ccol_cloexec_gate_ok)
    _ccol_cloexec_gate_ok =
        ccol_rw_lock_reinit_in_child(_ccol_cloexec_gate) == 0;
}

/* The prepare handlers of fork() run in the reverse order of their
 * registration, so the handler that registers first runs last. The gate must
 * run after every prepare handler of the library, because those handlers take
 * locks that a thread can hold while it waits to enter the gate: in the other
 * order a handler waits for such a thread, the thread waits for the gate,
 * and fork() never returns. ccol_at_fork therefore calls this before it
 * registers any handler, and the constructor below calls it at load, so no
 * order of constructors, of static linking or of first use can register a
 * handler of the library ahead of the gate. When the registration fails, the
 * gate runs anyway, and only the wait of fork() is absent. */
static void _ccol_cloexec_gate_do_register(void) {
  (void)_ccol_at_fork_raw(_ccol_cloexec_gate_prepare, _ccol_cloexec_gate_parent,
                          _ccol_cloexec_gate_child);
}

void _ccol_cloexec_gate_register(void) {
  ccol_call_once(_ccol_cloexec_gate_reg_once, _ccol_cloexec_gate_do_register);
}

__attribute__((constructor)) static void _ccol_cloexec_gate_at_load(void) {
  _ccol_cloexec_gate_register();
}
#endif /* _CCOL_CLOEXEC_GATE */
