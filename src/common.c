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

/* Returns the smallest power of two that is >= input, or ccol_invalid_size
 * when that power does not fit in a size_t. For an input of at least 2, the
 * answer is 2 to the power of the bit length of input - 1. One count of
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
   * length equal to the width means the answer is 2^width, which a size_t
   * cannot hold. */
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
   * the end. A hint of SIZE_MAX names a size that is not representable. The
   * function refuses it here, and does not let the code form hint + 1. That
   * expression wraps to 0. The 64-byte floor then wins, and the function
   * gives back a buffer much smaller than the caller asked for. It also
   * reports success. */
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

/* Doubles the capacity of the buffer until it holds 'needed' bytes. This
 * function sets b->oom if the reallocation fails, or if a size_t overflows. */
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
  /* The size that this append needs is b->len + n + 1. The code must test
   * that sum for representability BEFORE it forms the sum, and not after. The
   * sum wraps if the code forms it first. The wrapped value then compares
   * below b->cap, and no growth happens. The memcpy below then runs with the
   * enormous original n of the caller, against a buffer that has room for
   * none of it.
   *
   * A subtraction cannot wrap, because b->len is always below b->cap, and so
   * is at most SIZE_MAX - 1. The function refuses a request whose size is not
   * representable in the same way as a failed allocation. It latches oom, so
   * a caller that checks for an error only at the end of a build pass still
   * sees the failure. */
  if (n > SIZE_MAX - 1 - b->len) {
    b->oom = true;
    return;
  }
  size_t needed = b->len + n + 1;
  if (needed > b->cap) growbuf_grow(b, needed);
  if (b->oom) return;
  /* An empty append is already complete, because b->buf[b->len] holds the NUL
   * that every other path leaves. A return here also stops a (NULL, 0) append
   * from reaching memcpy. The pointer arguments of memcpy must be valid even
   * for a length of zero. A caller writes such an append naturally, because
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
 * written the copy. It never goes back. A thread claims only the first slot
 * that it sees EMPTY, after it saw every earlier slot claimed, so the claimed
 * slots always form a prefix of the table and a lookup stops at the first
 * EMPTY slot. */
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
    /* Another thread claimed this slot first. Look at it again: when it is
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

void _ccol_atfork_order_record(ccol_atfork_module_t ccol_module) {
  if ((unsigned)ccol_module >= (unsigned)ccol_atfork_module_count) return;

  /* There is no lock here, and this is deliberate. This code runs only inside
     a fork-prepare handler. Those handlers run one sequence at a time, under
     the atfork lock of the C library. There is no concurrent caller to
     exclude. A lock here would nest one more lock inside the handlers whose
     nesting this code exists to police. That is the last thing worth an
     addition to them. */
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
