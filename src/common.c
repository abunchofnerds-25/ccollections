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

#define POWERS_OF_TWO_LEN 64
static uint64_t uint64_powers_of_two[POWERS_OF_TWO_LEN] = {
    1ULL,
    2ULL,
    4ULL,
    8ULL,
    16ULL,
    32ULL,
    64ULL,
    128ULL,
    256ULL,
    512ULL,
    1024ULL,
    2048ULL,
    4096ULL,
    8192ULL,
    16384ULL,
    32768ULL,
    65536ULL,
    131072ULL,
    262144ULL,
    524288ULL,
    1048576ULL,
    2097152ULL,
    4194304ULL,
    8388608ULL,
    16777216ULL,
    33554432ULL,
    67108864ULL,
    134217728ULL,
    268435456ULL,
    536870912ULL,
    1073741824ULL,
    2147483648ULL,
    4294967296ULL,
    8589934592ULL,
    17179869184ULL,
    34359738368ULL,
    68719476736ULL,
    137438953472ULL,
    274877906944ULL,
    549755813888ULL,
    1099511627776ULL,
    2199023255552ULL,
    4398046511104ULL,
    8796093022208ULL,
    17592186044416ULL,
    35184372088832ULL,
    70368744177664ULL,
    140737488355328ULL,
    281474976710656ULL,
    562949953421312ULL,
    1125899906842624ULL,
    2251799813685248ULL,
    4503599627370496ULL,
    9007199254740992ULL,
    18014398509481984ULL,
    36028797018963968ULL,
    72057594037927936ULL,
    144115188075855872ULL,
    288230376151711744ULL,
    576460752303423488ULL,
    1152921504606846976ULL,
    2305843009213693952ULL,
    4611686018427387904ULL,
    9223372036854775808ULL};

/* Returns the smallest power of two that is >= _input, using binary search
 * over a precomputed table. The table covers all 64-bit powers of two, so the
 * result is architecture-dependent (capped at the pointer-size maximum).
 * Returns ccol_invalid_size when _input exceeds the largest representable
 * power of two for the current architecture. */
size_t ccol_find_nearest_gte_power_of_two(size_t _input) {
  uint64_t input = _input;
  size_t result = 0;

  if (input <= uint64_powers_of_two[0]) {
    result = (size_t)uint64_powers_of_two[0];
    return result;
  }

  int max_index = sizeof(size_t) * 8 - 1;  // Arch dependent

  if (input > uint64_powers_of_two[max_index]) {
    return ccol_invalid_size;
  }

  int left = 0, right = max_index, middle = (left + right) / 2;

  while (true) {
    if (uint64_powers_of_two[middle] == input) {
      // Found it
      result = (size_t)uint64_powers_of_two[middle];
      return result;
    } else if (uint64_powers_of_two[middle] < input) {
      // Go right
      left = middle + 1;
    } else {
      if (middle > 0 && uint64_powers_of_two[middle - 1] < input) {
        // Found it
        result = (size_t)uint64_powers_of_two[middle];
        return result;
      }
      // Go left
      right = middle;
    }

    middle = (left + right) / 2;
  }

  return ccol_invalid_size;
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
  size_t cap = hint + 1 > 64 ? hint + 1 : 64;
  b->m_procs = mp;
  b->buf = _ccol_mem_alloc(mp, cap);
  b->len = 0;
  b->cap = b->buf ? cap : 0;
  b->oom = b->buf ? false : true;
  if (b->buf) b->buf[0] = '\0';
}

/* Double the buffer capacity until it holds 'needed' bytes.
 * Sets b->oom on reallocation failure or size_t overflow. */
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
  if (b->len + n + 1 > b->cap) growbuf_grow(b, b->len + n + 1);
  if (b->oom) return;
  memcpy(b->buf + b->len, data, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

#ifdef RUNNING_UNIT_TESTS
/* See ccol_atfork_module_t in common.h for what this exists to catch. */
static const char *const _ccol_atfork_module_names[ccol_atfork_module_count] = {
    "clogger", "cthreadcomm", "cthreadpool", "chttpserver"};

/* The handlers that have already run in the fork currently being prepared, in
   the order they ran. */
static unsigned char _ccol_atfork_seq[ccol_atfork_module_count];
static unsigned _ccol_atfork_seq_len;

/* _ccol_atfork_ran_before[a][b] records that a's handler ran before b's in some
   earlier fork. */
static bool _ccol_atfork_ran_before[ccol_atfork_module_count]
                                   [ccol_atfork_module_count];

void _ccol_atfork_order_record(ccol_atfork_module_t ccol_module) {
  if ((unsigned)ccol_module >= (unsigned)ccol_atfork_module_count) return;

  /* No lock, deliberately. This runs only from inside a fork-prepare handler,
     and those run one sequence at a time under the C library's own atfork lock,
     so there is no concurrent caller to exclude. Taking a lock here would nest
     one more lock inside the very handlers whose nesting this exists to police,
     which is the thing least worth adding to them. */
  for (unsigned i = 0; i < _ccol_atfork_seq_len; i++) {
    if (_ccol_atfork_seq[i] == (unsigned char)ccol_module) {
      /* This handler is running a second time, so a new fork has begun and the
         recorded sequence belongs to the previous one. */
      _ccol_atfork_seq_len = 0;
      break;
    }
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
#endif /* RUNNING_UNIT_TESTS */
