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
 * Every case writes to /dev/null, so each figure describes the formatting, the
 * lock and the write path, and not the speed of the filesystem that holds this
 * repository.
 *
 * These cases cover three things. The first is the output format, because
 * logfmt, JSON and syslog do measurably different amounts of formatting work.
 * The second is synchronous against asynchronous, which is the difference
 * between a write by the calling thread and a handover of the record to the
 * writer thread. The third is the suppressed case, where the logger discards a
 * record below its own level: the price a program pays for the debug logging
 * that it has turned off.
 */

#include <clogger.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bench.h"

#define BENCH_LOG_N 50000

/* See the note on the thread count of the cache benchmarks. These cases are
 * loops over the records, with no fixed cost to spread out, so a shorter
 * repetition measures the same cost and leaves room for many more
 * repetitions. */
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
  log_state_t *st = calloc(1, sizeof(*st));
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
      .queue_size = 0, .flush_buffer_size = 0, .flush_interval_us = 0};
  return log_make(CLOG_FMT_LOGFMT, &cfg);
}

static void log_write_run(void *state, size_t n) {
  log_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    clog_info(st->logger, "request completed id=%zu status=%d bytes=%d", i, 200,
              1024);
  bench_sink(&st->logger);
}

/* The asynchronous writer thread is still draining the queue when the loop
 * ends, so the flush is inside the measurement. Without it, this case would
 * report only the cost of the handover to the queue, and it would look better
 * the further the writer thread fell behind. */
static void log_write_async_run(void *state, size_t n) {
  log_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    clog_info(st->logger, "request completed id=%zu status=%d bytes=%d", i, 200,
              1024);
  clog_flush(st->logger);
  bench_sink(&st->logger);
}

/* This is the caller half of the asynchronous path, with no flush: what the
 * logging thread itself pays to hand a record over. The harness reports it
 * beside the case above, which includes the flush, because the two answer
 * different questions. On its own, the case with the flush reads as
 * asynchronous logging that is slower than synchronous logging, while what it
 * really shows is that the total work is larger and the share of the caller is
 * smaller. */
static void log_write_async_caller_run(void *state, size_t n) {
  log_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    clog_info(st->logger, "request completed id=%zu status=%d bytes=%d", i, 200,
              1024);
  bench_sink(&st->logger);
}

/* CLOG_DEBUG is below the CLOG_INFO threshold of this logger, so the logger
 * discards every one of these records. */
static void log_suppressed_run(void *state, size_t n) {
  log_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    clog_debug(st->logger, "request completed id=%zu status=%d bytes=%d", i,
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
