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

// Give the version of the loaded library, as a number and as a string. A
// program calls them to learn which release the dynamic loader picked, since
// the macros only tell the version of the header it compiled with.
unsigned int ccol_version(void) { return CCOL_VERSION_NUMBER; }

const char *ccol_version_string(void) { return CCOL_VERSION_STRING; }

// CCOL_COMMON_TSAN is set in a ThreadSanitizer build (GCC or Clang), where the
// child of fork() annotates the rwlock it initializes again.
#if defined(__SANITIZE_THREAD__)
#define CCOL_COMMON_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CCOL_COMMON_TSAN 1
#endif
#endif
#if defined(CCOL_COMMON_TSAN)
int __tsan_mutex_pre_unlock(void *addr, unsigned flags);
void __tsan_mutex_post_unlock(void *addr, unsigned flags);
#endif

// Initializes again, in the child of fork(), an rwlock that a prepare handler
// write-locked. The fork child handlers of every module need it, because a
// plain unlock in the child doesn't release such a lock. lock points at the
// rwlock (void * because strict C11 has no rwlock type). Gives 0 or an error
// number. ThreadSanitizer doesn't model the re-init, so in a TSan build we
// first tell it the write lock is released (the two unlock annotations with no
// unlock between them); the init then makes a new lock in its model too.
int ccol__rw_lock_reinit_in_child_impl(void *lock) {
  ccol__rw_lock *rw = lock;
#if defined(CCOL_COMMON_TSAN)
  (void)__tsan_mutex_pre_unlock(rw, 0);
  __tsan_mutex_post_unlock(rw, 0);
#endif
  memset(rw, 0, sizeof(*rw));
  return ccol__rw_lock_init(*rw);
}

// Gives the smallest power of two that is >= input, or ccol_invalid_size when
// it doesn't fit in a size_t. The containers use it to round a requested
// capacity to the power-of-two sizes they grow by. One count of leading zeros
// of input - 1 gives the exponent, which needs unsigned long as wide as size_t;
// that holds on every target we support (LP64 and ILP32).
_Static_assert(sizeof(unsigned long) == sizeof(size_t),
               "the power-of-two rounding needs size_t as wide as long");
size_t ccol__find_nearest_gte_power_of_two(size_t input) {
  if (input <= 1) return 1;
  unsigned long below = (unsigned long)(input - 1);
  int bits = (int)(sizeof(unsigned long) * CHAR_BIT) - __builtin_clzl(below);
  if (bits >= (int)(sizeof(unsigned long) * CHAR_BIT)) return ccol_invalid_size;
  return (size_t)1 << bits;
}

// Growable byte buffer.
//
// Initializes b as an empty string in a 256-byte buffer allocated through mp.
// The serializers and the logger's record builder start their output buffer
// with it. On a failed allocation the buffer is NULL and b->oom is set, which
// every later append respects.
void ccol_growbuf_init(ccol_growbuf *b, ccol_memmgmt_procs *mp) {
  b->m_procs = mp;
  b->buf = ccol__mem_alloc(mp, 256);
  b->len = 0;
  b->cap = b->buf ? 256 : 0;
  b->oom = b->buf ? false : true;
  if (b->buf) b->buf[0] = '\0';
}

// Initializes b like ccol_growbuf_init, but with room for hint bytes plus the
// NUL (64 at least), for a caller that knows roughly how much it will write and
// wants to skip the growth steps. A failed allocation, or a hint of SIZE_MAX
// (refused before hint + 1 can wrap to a tiny buffer), leaves the buffer NULL
// and sets b->oom.
void ccol_growbuf_init_hint(ccol_growbuf *b, ccol_memmgmt_procs *mp,
                            size_t hint) {
  b->m_procs = mp;
  b->len = 0;
  if (hint == SIZE_MAX) {
    b->buf = NULL;
    b->cap = 0;
    b->oom = true;
    return;
  }
  size_t cap = hint + 1 > 64 ? hint + 1 : 64;
  b->buf = ccol__mem_alloc(mp, cap);
  b->cap = b->buf ? cap : 0;
  b->oom = b->buf ? false : true;
  if (b->buf) b->buf[0] = '\0';
}

// Doubles the capacity of b until it holds needed bytes, for
// ccol_growbuf_append_slow. It does nothing when b->oom is set, and sets b->oom
// when the realloc fails or the size would overflow a size_t, leaving the old
// buffer in place.
static void growbuf_grow(ccol_growbuf *b, size_t needed) {
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
  char *p = ccol__mem_realloc(b->m_procs, b->buf, new_cap);
  if (!p) {
    b->oom = true;
    return;
  }
  b->buf = p;
  b->cap = new_cap;
}

// Appends n bytes of data to b and keeps the buffer NUL-terminated. It's the
// out-of-line half of the inline ccol_growbuf_append, which handles the common
// case of an append that fits; this one takes growth, empty appends and the oom
// state. It does nothing once b->oom is set, and sets it when the size
// overflows or growth fails. The size check runs before len + n + 1 is formed,
// so a huge n latches oom instead of wrapping into an undersized copy. An empty
// append returns before memcpy, because data may be NULL then.
void ccol_growbuf_append_slow(ccol_growbuf *b, const char *data, size_t n) {
  if (b->oom) return;
  if (n > SIZE_MAX - 1 - b->len) {
    b->oom = true;
    return;
  }
  size_t needed = b->len + n + 1;
  if (needed > b->cap) growbuf_grow(b, needed);
  if (b->oom) return;
  if (n == 0) return;
  memcpy(b->buf + b->len, data, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

// Allocator procs intern table.
//
// The state of one slot. A slot goes from EMPTY to WRITING when a thread claims
// it and from WRITING to READY when the copy is written, and never back. A
// thread claims only the first EMPTY slot after seeing every earlier one READY,
// so the claimed slots are a prefix and a lookup can stop at the first EMPTY.
enum {
  CCOL_PROCS_SLOT_EMPTY = 0,
  CCOL_PROCS_SLOT_WRITING = 1,
  CCOL_PROCS_SLOT_READY = 2
};

// The process-lifetime copies of the procs structs and the state of each slot.
static ccol_memmgmt_procs ccol_procs_slots[CCOL_PROCS_INTERN_CAPACITY];
static atomic_uchar ccol_procs_slot_state[CCOL_PROCS_INTERN_CAPACITY];
#ifdef RUNNING_UNIT_TESTS
// Test hooks: a delay between the claim and the publish of a slot (or, with
// claim_wait_sightings set, the bound of a wait for that many sightings of a
// slot being written), and the count of such sightings.
_Atomic unsigned ccol__procs_intern_claim_delay_us_for_tests;
_Atomic unsigned ccol__procs_intern_claim_wait_sightings_for_tests;
_Atomic unsigned ccol__procs_intern_writing_sightings_for_tests;
#endif

// True when the two procs structs hold the same four functions. The intern
// table uses it to find an existing copy of a caller's allocator.
static inline bool ccol_procs_same(const ccol_memmgmt_procs *a,
                                   const ccol_memmgmt_procs *b) {
  return a->malloc == b->malloc && a->free == b->free &&
         a->calloc == b->calloc && a->realloc == b->realloc;
}

// True when mp points at a slot of the intern table. Containers use it to keep
// an interned pointer as it is instead of copying the struct. It compares
// integers, because a relational comparison of pointers into different objects
// is undefined.
bool ccol_procs_is_interned(const ccol_memmgmt_procs *mp) {
  uintptr_t addr = (uintptr_t)mp;
  uintptr_t base = (uintptr_t)&ccol_procs_slots[0];
  return addr >= base && addr < base + sizeof(ccol_procs_slots) &&
         (addr - base) % sizeof(ccol_memmgmt_procs) == 0;
}

// Waits until slot i, claimed by another thread, is READY in
// ccol_procs_slot_state. ccol_procs_intern needs it because the slot may hold
// the very content the lookup wants, and skipping it would give one content two
// slots. It spins a little, then sleeps a microsecond at a time so a
// descheduled claimer gets the CPU.
static __attribute__((noinline, cold)) void ccol_procs_wait_ready(size_t i) {
  unsigned spins = 0;
  while (atomic_load_explicit(&ccol_procs_slot_state[i],
                              memory_order_acquire) != CCOL_PROCS_SLOT_READY) {
    if (spins < 64) {
      spins++;
    } else {
      struct timespec ts = {0, 1000L};
      nanosleep(&ts, NULL);
    }
  }
}

// Gives the process-lifetime copy of the allocator mp in *out, so cjson and
// cyaml nodes never point at the caller's struct. It finds a slot with the same
// four functions or claims the next EMPTY slot, lock free, in ccol_procs_slots
// and ccol_procs_slot_state. A NULL mp gives NULL, and a pointer into the table
// is returned as it is. The caller's struct is read once into a local so every
// comparison and the copy see the same values. Gives ccol_success, or
// ccol_container_full when the table is full.
ccol_retval ccol_procs_intern(ccol_memmgmt_procs *mp,
                              ccol_memmgmt_procs **out) {
  *out = NULL;
  if (mp == NULL) return ccol_success;
  if (ccol_procs_is_interned(mp)) {
    *out = mp;
    return ccol_success;
  }
  ccol_memmgmt_procs want;
  memcpy(&want, mp, sizeof(want));
  for (size_t i = 0; i < CCOL_PROCS_INTERN_CAPACITY; i++) {
    unsigned char st =
        atomic_load_explicit(&ccol_procs_slot_state[i], memory_order_acquire);
    if (st == CCOL_PROCS_SLOT_EMPTY) {
      unsigned char expected = CCOL_PROCS_SLOT_EMPTY;
      if (atomic_compare_exchange_strong_explicit(
              &ccol_procs_slot_state[i], &expected, CCOL_PROCS_SLOT_WRITING,
              memory_order_acq_rel, memory_order_acquire)) {
#ifdef RUNNING_UNIT_TESTS
        unsigned delay_us = atomic_load_explicit(
            &ccol__procs_intern_claim_delay_us_for_tests, memory_order_relaxed);
        unsigned sightings = atomic_load_explicit(
            &ccol__procs_intern_claim_wait_sightings_for_tests,
            memory_order_relaxed);
        if (delay_us && sightings) {
          for (unsigned waited = 0;
               waited < delay_us &&
               atomic_load(&ccol__procs_intern_writing_sightings_for_tests) <
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
        ccol_procs_slots[i] = want;
        atomic_store_explicit(&ccol_procs_slot_state[i], CCOL_PROCS_SLOT_READY,
                              memory_order_release);
        *out = &ccol_procs_slots[i];
        return ccol_success;
      }
      st = expected;
    }
#ifdef RUNNING_UNIT_TESTS
    if (st == CCOL_PROCS_SLOT_WRITING)
      atomic_fetch_add(&ccol__procs_intern_writing_sightings_for_tests, 1);
#endif
    if (st == CCOL_PROCS_SLOT_WRITING) ccol_procs_wait_ready(i);
    if (ccol_procs_same(&ccol_procs_slots[i], &want)) {
      *out = &ccol_procs_slots[i];
      return ccol_success;
    }
  }
  return ccol_container_full;
}

#ifdef RUNNING_UNIT_TESTS
// Test hooks for the intern table: the first one counts the claimed slots, the
// second empties every slot from keep on, so a test can fill the table and put
// it back.
size_t ccol__procs_intern_used_for_tests(void) {
  size_t n = 0;
  while (n < CCOL_PROCS_INTERN_CAPACITY &&
         atomic_load_explicit(&ccol_procs_slot_state[n],
                              memory_order_acquire) != CCOL_PROCS_SLOT_EMPTY)
    n++;
  return n;
}

void ccol__procs_intern_truncate_for_tests(size_t keep) {
  for (size_t i = keep; i < CCOL_PROCS_INTERN_CAPACITY; i++) {
    memset(&ccol_procs_slots[i], 0, sizeof(ccol_procs_slots[i]));
    atomic_store_explicit(&ccol_procs_slot_state[i], CCOL_PROCS_SLOT_EMPTY,
                          memory_order_release);
  }
}
#endif

#ifdef RUNNING_UNIT_TESTS
// Fork-handler order recorder (test builds only). The names of the modules, for
// the fatal messages.
static const char *const ccol_atfork_module_names[ccol__atfork_module_count] = {
    "clogger", "cthreadcomm", "cthreadpool", "chttpserver", "chttpclient"};

// The handlers that already ran in the fork being prepared, in order.
// ccol__atfork_order_reset empties it after every fork.
static unsigned char ccol_atfork_seq[ccol__atfork_module_count];
static unsigned ccol_atfork_seq_len;

// ccol_atfork_ran_before[a][b] records that a's handler ran before b's in an
// earlier fork.
static bool ccol_atfork_ran_before[ccol__atfork_module_count]
                                  [ccol__atfork_module_count];

#if defined(CCOL__CLOEXEC_GATE)
extern bool ccol__cloexec_gate_prepared_for_tests;
#endif

// Records that the prepare handler of ccol_module started. Every module's
// prepare handler calls it in a test build, to prove that no two handlers run
// in opposite orders in two forks (a real lock cycle) and that none runs after
// the close-on-exec gate; either case ends the process. It reads
// ccol__cloexec_gate_prepared_for_tests and updates ccol_atfork_seq and
// ccol_atfork_ran_before. It takes no lock: prepare handlers run one sequence
// at a time under the C library's atfork lock, and a lock here would nest
// inside the very handlers it checks. A handler already in the sequence was
// registered twice and adds nothing.
void ccol__atfork_order_record(ccol__atfork_module ccol_module) {
  if ((unsigned)ccol_module >= (unsigned)ccol__atfork_module_count) return;
#if defined(CCOL__CLOEXEC_GATE)
  if (ccol__cloexec_gate_prepared_for_tests)
    ccol__fatal_err(
        "fork-prepare handler of %s ran after the close-on-exec gate, so a "
        "fork() can wait for a lock whose holder waits for the gate",
        ccol_atfork_module_names[ccol_module]);
#endif

  for (unsigned i = 0; i < ccol_atfork_seq_len; i++) {
    if (ccol_atfork_seq[i] == (unsigned char)ccol_module) return;
  }

  for (unsigned i = 0; i < ccol_atfork_seq_len; i++) {
    ccol__atfork_module earlier = (ccol__atfork_module)ccol_atfork_seq[i];
    if (ccol_atfork_ran_before[ccol_module][earlier]) {
      ccol__fatal_err(
          "fork-prepare handler order inverted: %s ran before %s in an earlier "
          "fork and after it in this one, so the locks the two handlers hold "
          "nest in opposite orders between two forks",
          ccol_atfork_module_names[ccol_module],
          ccol_atfork_module_names[earlier]);
    }
    ccol_atfork_ran_before[earlier][ccol_module] = true;
  }

  if (ccol_atfork_seq_len < (unsigned)ccol__atfork_module_count)
    ccol_atfork_seq[ccol_atfork_seq_len++] = (unsigned char)ccol_module;
}

// Empties ccol_atfork_seq, so the next record starts the sequence of a new
// fork. The recorder's parent and child handlers call it after each fork.
void ccol__atfork_order_reset(void) { ccol_atfork_seq_len = 0; }

// Registers ccol__atfork_order_reset as the parent and child handler, so every
// fork starts with an empty sequence; guessing the start of a fork from a
// handler running again would lose pairs. It runs from a constructor so it
// comes ahead of every fork and every module handler.
__attribute__((constructor)) static void ccol_atfork_order_register(void) {
  if (ccol__at_fork(NULL, ccol__atfork_order_reset, ccol__atfork_order_reset) !=
      0) {
    ccol__fatal_err("cannot register the fork-prepare order recorder");
  }
}
#endif

#if defined(CCOL__CLOEXEC_GATE)
// The close-on-exec gate (see common.h). A thread that makes a descriptor in
// two steps holds the read side across both, and the fork-prepare handler takes
// the write side, so fork() waits until every such descriptor is close-on-exec.
// A reader waits only for a fork and takes no other lock inside.
// ccol_cloexec_gate_ok is false when the lock couldn't be initialized; the gate
// then does nothing.
static ccol__rw_lock ccol_cloexec_gate;
static ccol__once_flag ccol_cloexec_gate_once = CCOL__ONCE_INIT;
static ccol__once_flag ccol_cloexec_gate_reg_once = CCOL__ONCE_INIT;
static bool ccol_cloexec_gate_ok;

#ifdef RUNNING_UNIT_TESTS
// Test counters of entries and exits, so a test can check every creation site.
atomic_ulong ccol__cloexec_gate_enters_for_tests;
atomic_ulong ccol__cloexec_gate_leaves_for_tests;
// True from the gate's prepare handler until its parent or child handler;
// ccol__atfork_order_record reads it. Only prepare handlers run in that window,
// so it needs no lock.
bool ccol__cloexec_gate_prepared_for_tests;
// Prepare handler starts, counted before the wait for the write side.
atomic_ulong ccol__cloexec_gate_prepares_for_tests;
#endif

// Initializes ccol_cloexec_gate once, on the first enter or prepare, and
// records in ccol_cloexec_gate_ok whether that worked.
static void ccol_cloexec_gate_init(void) {
  ccol_cloexec_gate_ok = ccol__rw_lock_init(ccol_cloexec_gate) == 0;
}

// Takes the read side of the close-on-exec gate. A site that makes a descriptor
// in two steps calls it first, so no fork() lands between the steps. It does
// nothing when the lock couldn't be initialized, and counts the entry in a test
// build.
void ccol__cloexec_gate_enter(void) {
  ccol__call_once(ccol_cloexec_gate_once, ccol_cloexec_gate_init);
  if (ccol_cloexec_gate_ok) ccol__rw_lock_rdlock(ccol_cloexec_gate);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&ccol__cloexec_gate_enters_for_tests, 1,
                            memory_order_relaxed);
#endif
}

// Releases the read side of the gate once the descriptor is close-on-exec, and
// counts the exit in a test build.
void ccol__cloexec_gate_leave(void) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&ccol__cloexec_gate_leaves_for_tests, 1,
                            memory_order_relaxed);
#endif
  if (ccol_cloexec_gate_ok) ccol__rw_lock_unlock(ccol_cloexec_gate);
}

// Fork-prepare handler of the gate: takes the write side, so fork() waits for
// every thread between the two steps of a descriptor creation. It sets
// ccol__cloexec_gate_prepared_for_tests in a test build.
static void ccol_cloexec_gate_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&ccol__cloexec_gate_prepares_for_tests, 1);
#endif
  ccol__call_once(ccol_cloexec_gate_once, ccol_cloexec_gate_init);
  if (ccol_cloexec_gate_ok) ccol__rw_lock_wrlock(ccol_cloexec_gate);
#ifdef RUNNING_UNIT_TESTS
  ccol__cloexec_gate_prepared_for_tests = true;
#endif
}

// Fork parent handler of the gate: clears the prepared flag and releases the
// write side, so the parent's threads can make descriptors again.
static void ccol_cloexec_gate_parent(void) {
#ifdef RUNNING_UNIT_TESTS
  ccol__cloexec_gate_prepared_for_tests = false;
#endif
  if (ccol_cloexec_gate_ok) ccol__rw_lock_unlock(ccol_cloexec_gate);
}

// Fork child handler of the gate: clears the prepared flag and initializes the
// gate lock again, since in the child a plain unlock doesn't release a lock
// that the forking thread took (see ccol__rw_lock_reinit_in_child). A failure
// sets ccol_cloexec_gate_ok to false, which turns the gate off.
static void ccol_cloexec_gate_child(void) {
#ifdef RUNNING_UNIT_TESTS
  ccol__cloexec_gate_prepared_for_tests = false;
#endif
  if (ccol_cloexec_gate_ok)
    ccol_cloexec_gate_ok =
        ccol__rw_lock_reinit_in_child(ccol_cloexec_gate) == 0;
}

// Registers the gate's fork handlers. The gate must run after every library
// prepare handler, because a thread can hold a module lock while it waits to
// enter the gate, and prepare handlers run in reverse registration order; so
// the gate must register first. A failed registration leaves the gate working,
// only fork() doesn't wait for it.
static void ccol_cloexec_gate_do_register(void) {
  (void)ccol__at_fork_raw(ccol_cloexec_gate_prepare, ccol_cloexec_gate_parent,
                          ccol_cloexec_gate_child);
}

// Registers the gate's fork handlers once. ccol__at_fork calls it before every
// registration of the library, so the gate is always the first one in.
void ccol__cloexec_gate_register(void) {
  ccol__call_once(ccol_cloexec_gate_reg_once, ccol_cloexec_gate_do_register);
}

// Registers the gate at load, so no constructor order, static link or first use
// can put a module handler ahead of it.
__attribute__((constructor)) static void ccol_cloexec_gate_at_load(void) {
  ccol__cloexec_gate_register();
}
#endif
