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
 * @file cdeadline.h
 * @brief INTERNAL ONLY. The conversion of a public timeout, a uint64_t count
 *        of microseconds, into an absolute CLOCK_MONOTONIC deadline and into
 *        the coarser units that system calls take.
 *
 * Every public timeout, interval and duration of the library is a uint64_t
 * count of microseconds. Every module that turns one into a deadline or into
 * a millisecond count goes through these helpers, so that all of them
 * saturate the same way: a value too large to represent becomes the largest
 * representable deadline, which no realistic wait reaches, and never wraps
 * into a deadline in the past. A wrapped deadline would turn a request to
 * wait very long into a wait that ends at once.
 *
 * All the arithmetic runs in uint64_t, whatever the widths of long and
 * time_t are, so that it is the same on ILP32 and on LP64.
 *
 * This header is internal. Every function is static inline, so it adds no
 * symbol to the library. make install does not install it.
 */

#ifndef CCOL_CDEADLINE_H
#define CCOL_CDEADLINE_H

#include <limits.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stdint.h>
#include <time.h>

/* The largest value of the signed integer type time_t. The C library offers
 * no macro for it. The shift reads the width of time_t off sizeof, so the
 * value is right for a 4-byte time_t on ILP32 and for the 8-byte one of LP64
 * and of _TIME_BITS=64. */
#define CCOL_DEADLINE_TIME_T_MAX \
  ((time_t)(~(uintmax_t)0 >>     \
            (sizeof(uintmax_t) * CHAR_BIT - sizeof(time_t) * CHAR_BIT + 1)))

/**
 * Adds us microseconds to *ts. *ts must hold a tv_nsec in [0, 999999999],
 * as clock_gettime() gives it, and a tv_sec that is not negative. A sum past
 * the largest time_t saturates to the largest representable time.
 */
static inline void ccol_timespec_add_us(struct timespec *ts, uint64_t us) {
  uint64_t secs = us / UINT64_C(1000000);
  long nsec = (long)(us % UINT64_C(1000000)) * 1000L;
  /* Both parts are in [0, 999999999], so their sum fits a 4-byte long and
   * carries at most one whole second. */
  ts->tv_nsec += nsec;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_nsec -= 1000000000L;
    secs++;
  }
  uint64_t headroom = ts->tv_sec >= 0
                          ? (uint64_t)(CCOL_DEADLINE_TIME_T_MAX - ts->tv_sec)
                          : (uint64_t)CCOL_DEADLINE_TIME_T_MAX;
  if (secs > headroom) {
    ts->tv_sec = CCOL_DEADLINE_TIME_T_MAX;
    ts->tv_nsec = 999999999L;
  } else {
    ts->tv_sec = (time_t)((uint64_t)ts->tv_sec + secs);
  }
}

/**
 * Sets *abs to the current CLOCK_MONOTONIC time plus us microseconds, with
 * the saturation of ccol_timespec_add_us(). Returns false only when
 * clock_gettime() fails.
 */
static inline bool ccol_deadline_after_us(uint64_t us, struct timespec *abs) {
  if (clock_gettime(CLOCK_MONOTONIC, abs) != 0) return false;
  ccol_timespec_add_us(abs, us);
  return true;
}

/**
 * Converts a microsecond count to whole milliseconds, rounded UP, so that a
 * wait of the result never ends before the duration of the input, and a
 * positive input never becomes 0.
 */
static inline uint64_t ccol_us_to_ms_ceil(uint64_t us) {
  return us / UINT64_C(1000) + (us % UINT64_C(1000) != 0);
}

/**
 * Converts a microsecond count to whole seconds, rounded UP, with the same
 * reasons as ccol_us_to_ms_ceil().
 */
static inline uint64_t ccol_us_to_s_ceil(uint64_t us) {
  return us / UINT64_C(1000000) + (us % UINT64_C(1000000) != 0);
}

/**
 * Gives the time from now to *deadline in whole milliseconds, rounded UP and
 * capped at INT_MAX, as a timeout for poll(2) or epoll_wait(2). A deadline
 * that has passed gives 0, which is a poll that looks once and does not
 * block, never a timeout without a look. A wait of the result never ends
 * before the deadline.
 */
static inline int ccol_deadline_remaining_ms_ceil(
    const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (deadline->tv_sec < now.tv_sec ||
      (deadline->tv_sec == now.tv_sec && deadline->tv_nsec <= now.tv_nsec))
    return 0;
  /* deadline > now here, so the second difference is not negative, and the
   * nanosecond difference lies strictly between -1 and 1 second. */
  uint64_t secs = (uint64_t)deadline->tv_sec - (uint64_t)now.tv_sec;
  if (secs > (uint64_t)INT_MAX / 1000u) return INT_MAX;
  int64_t ns = (int64_t)secs * 1000000000LL +
               ((int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec);
  int64_t ms = (ns + 999999LL) / 1000000LL;
  return ms > INT_MAX ? INT_MAX : (int)ms;
}

#endif /* CCOL_CDEADLINE_H */
