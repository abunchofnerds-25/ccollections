/*
 * MIT License
 *
 * Copyright (c) 2026 - A bunch of nerds
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file bench_logging.c
 * @brief Benchmarks for clogger.
 *
 * Every case writes to /dev/null so that the figure describes formatting,
 * locking and the write path rather than the speed of whatever filesystem the
 * repository happens to sit on. Three dimensions are covered: output format,
 * since logfmt, JSON and syslog do measurably different amounts of formatting
 * work; synchronous against asynchronous, which is the difference between the
 * calling thread doing the write and it handing the record to the writer
 * thread; and the suppressed case, where a record below the logger's level is
 * discarded, which is the cost a program pays for debug logging it has turned
 * off.
 */

#include <clogger.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bench.h"

#define BENCH_LOG_N 50000

/* See the note on the cache benchmarks' own multi-threaded count: these are
 * per-record loops with nothing fixed to amortize, so a shorter repetition
 * measures the same cost and leaves room for many more repetitions. */
#define BENCH_LOG_MT_N 5000

typedef struct {
  clog logger;
  int fd;
} log_state_t;

static void log_teardown(void *state) {
  log_state_t *st = state;
  if (st->logger != CLOG_INVALID) clog_close(st->logger);
  if (st->fd >= 0) close(st->fd);
  free(st);
}

static log_state_t *log_make(clog_format_t fmt, const clog_async_cfg_t *async) {
  log_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->fd = open("/dev/null", O_WRONLY);
  if (st->fd < 0) {
    free(st);
    return NULL;
  }
  st->logger = clog_open_fd(st->fd, CLOG_INFO, async);
  if (st->logger == CLOG_INVALID) {
    log_teardown(st);
    return NULL;
  }
  clog_set_format(st->logger, fmt);
  return st;
}

static void *log_logfmt_setup(size_t n) {
  (void)n;
  return log_make(CLOG_FMT_LOGFMT, NULL);
}

static void *log_json_setup(size_t n) {
  (void)n;
  return log_make(CLOG_FMT_JSON, NULL);
}

static void *log_async_setup(size_t n) {
  (void)n;
  static const clog_async_cfg_t cfg = {
      .queue_size = 0, .flush_buffer_size = 0, .flush_interval_ms = 0};
  return log_make(CLOG_FMT_LOGFMT, &cfg);
}

static void log_write_run(void *state, size_t n) {
  log_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    ccol_log_info(st->logger, "request completed id=%zu status=%d bytes=%d", i,
                  200, 1024);
  bench_sink(&st->logger);
}

/* The async writer thread is still draining when the loop ends, so the flush is
 * inside the measurement: without it the case would report only the cost of
 * handing records to the queue, and would look better the further behind the
 * writer thread fell. */
static void log_write_async_run(void *state, size_t n) {
  log_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    ccol_log_info(st->logger, "request completed id=%zu status=%d bytes=%d", i,
                  200, 1024);
  clog_flush(st->logger);
  bench_sink(&st->logger);
}

/* The caller-side half of the asynchronous path, with no flush: what the
 * logging thread itself pays to hand a record over. Reported alongside the
 * flush-inclusive case above because the two answer different questions, and
 * the flush-inclusive one on its own reads as asynchronous logging being
 * slower than synchronous, when what it shows is that the total work is
 * larger while the caller's share of it is smaller. */
static void log_write_async_caller_run(void *state, size_t n) {
  log_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    ccol_log_info(st->logger, "request completed id=%zu status=%d bytes=%d", i,
                  200, 1024);
  bench_sink(&st->logger);
}

/* CLOG_DEBUG is below the logger's CLOG_INFO threshold, so every one of these
 * is discarded. */
static void log_suppressed_run(void *state, size_t n) {
  log_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    ccol_log_debug(st->logger, "request completed id=%zu status=%d bytes=%d", i,
                   200, 1024);
  bench_sink(&st->logger);
}

/* One logger, many threads, which is how a logger is actually used: the whole
 * point of these cases is the contention on the shared write path. */
BENCH_MT_SETUP(log_logfmt_setup)
BENCH_MT_SETUP(log_async_setup)

void bench_register_logging(void) {
  bench_add(&(bench_case_t){.group = "clogger",
                            .name = "write_logfmt_sync",
                            .setup = log_logfmt_setup,
                            .run = log_write_run,
                            .teardown = log_teardown,
                            .n = BENCH_LOG_N});
  bench_add_mt(&(bench_case_t){.group = "clogger",
                               .name = "write_logfmt_sync",
                               .setup_mt = log_logfmt_setup_mt,
                               .run = log_write_run,
                               .teardown = log_teardown,
                               .n = BENCH_LOG_MT_N,
                               .shared_fixture = true});
  bench_add(&(bench_case_t){.group = "clogger",
                            .name = "write_json_sync",
                            .setup = log_json_setup,
                            .run = log_write_run,
                            .teardown = log_teardown,
                            .n = BENCH_LOG_N});
  bench_add(&(bench_case_t){.group = "clogger",
                            .name = "write_logfmt_async",
                            .setup = log_async_setup,
                            .run = log_write_async_run,
                            .teardown = log_teardown,
                            .n = BENCH_LOG_N});
  bench_add(&(bench_case_t){.group = "clogger",
                            .name = "write_logfmt_async_caller_side",
                            .setup = log_async_setup,
                            .run = log_write_async_caller_run,
                            .teardown = log_teardown,
                            .n = BENCH_LOG_N});
  bench_add_mt(&(bench_case_t){.group = "clogger",
                               .name = "write_logfmt_async_caller_side",
                               .setup_mt = log_async_setup_mt,
                               .run = log_write_async_caller_run,
                               .teardown = log_teardown,
                               .n = BENCH_LOG_MT_N,
                               .shared_fixture = true});
  bench_add(&(bench_case_t){.group = "clogger",
                            .name = "suppressed_below_level",
                            .setup = log_logfmt_setup,
                            .run = log_suppressed_run,
                            .teardown = log_teardown,
                            .n = BENCH_LOG_N});
  bench_add_mt(&(bench_case_t){.group = "clogger",
                               .name = "suppressed_below_level",
                               .setup_mt = log_logfmt_setup_mt,
                               .run = log_suppressed_run,
                               .teardown = log_teardown,
                               .n = BENCH_LOG_MT_N,
                               .shared_fixture = true});
}
