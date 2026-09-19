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
 * @file bench_concurrency.c
 * @brief Benchmarks for cthreadpool and the cthreadcomm message primitives.
 *
 * Two shapes are measured for every queue type. The single-threaded round trip
 * sends and receives on one thread, so it reports the primitive's own
 * bookkeeping and locking with no contention and no blocking at all. The
 * producer and consumer pair reports what the same primitive costs when a
 * sender and a receiver genuinely contend and have to hand off through the
 * condition variable, which is the number that matters for a real pipeline and
 * the one a scheduling change moves.
 *
 * Messages are recycled rather than freshly allocated: a send hands ownership
 * to the queue and a receive hands it back, so the same buffer can go round
 * indefinitely. That keeps the allocator out of a measurement that is meant to
 * be about the queue.
 */

#include <cthreadcomm.h>
#include <cthreadpool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench.h"

#ifdef BENCH_HAVE_GLIB
#include <glib.h>
#endif

#define BENCH_QUEUE_N 200000

/* A shorter repetition for the contended queue variants, for the same reason
 * the cache benchmarks use one: a send/receive pair is a per-operation cost
 * with nothing fixed around it, and many short repetitions describe the
 * distribution far better than ten long ones.
 *
 * The thread pool's own variants keep the full count instead: their repetition
 * ends with a drain of the queue, which is a fixed cost per repetition rather
 * than per task, so shortening one would inflate the per-task figure and stop
 * it being comparable to the single-threaded case. */
#define BENCH_QUEUE_MT_N 20000
#define BENCH_POOL_N 100000
#define BENCH_QUEUE_CAPACITY 1024

/* ------------------------------------------------------------------------ */
/* cthreadpool                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
  ctpool pool;
  atomic_size_t counter;
} pool_state_t;

/* The task body is deliberately trivial. What is being measured is the cost of
 * getting work to a worker and accounting for its completion, so any real work
 * inside the task would only dilute that with something the pool does not
 * control. */
static void pool_task(void *arg) {
  pool_state_t *st = arg;
  atomic_fetch_add_explicit(&st->counter, 1, memory_order_relaxed);
}

static void pool_teardown(void *state) {
  pool_state_t *st = state;
  if (st->pool != CTPOOL_INVALID) {
    ctpool p = st->pool;
    ctpool_destroy(p);
  }
  free(st);
}

static void *pool_setup_n(size_t workers) {
  pool_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  atomic_init(&st->counter, 0);
  st->pool = ccol_create_cthread_pool(workers, 0, NULL);
  if (st->pool == CTPOOL_INVALID) {
    free(st);
    return NULL;
  }
  return st;
}

static void *pool_setup_1(size_t n) {
  (void)n;
  return pool_setup_n(1);
}

static void *pool_setup_4(size_t n) {
  (void)n;
  return pool_setup_n(4);
}

static void pool_submit_run(void *state, size_t n) {
  pool_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    /* Checked for the same reason every other allocation in this harness is: a
     * submit that fails is far cheaper than one that queues, so an unchecked
     * loop keeps timing and reports the failure as speed. */
    if (ctpool_submit(st->pool, pool_task, st, NULL) != ccol_success)
      bench_die("ctpool_submit failed");
  }
  /* The wait is inside the timed region on purpose: a submit that merely
   * enqueues is only half the operation, and stopping the clock before the
   * queue drains would report a number that improves whenever the pool gets
   * slower at actually running the work. */
  ctpool_wait(st->pool);
  bench_sink(&st->counter);
}

/* ------------------------------------------------------------------------ */
/* cthreadcomm queues                                                        */
/* ------------------------------------------------------------------------ */

typedef struct {
  ccol_circular_queue *cq;
  ccol_dynamic_queue *dq;
  ccol_channel *ch;
  void *payload;
  size_t payload_size;
} q_state_t;

#define BENCH_MSG_SIZE 64

/* Destroying a queue with messages still in it is a caller error the library
 * aborts on, and every message here carries one of the state's own buffers
 * rather than an allocation of its own, so draining means discarding the
 * pointers, not freeing them. A case that stops early (a thread that could not
 * be created, a send that succeeded with the matching receive left undone)
 * leaves exactly that, and without this the harness would abort on the one
 * path it was written to survive. */
static void q_drain(q_state_t *st) {
  c_message_t out = {0};
  /* No channel arm: a channel routes by thread identity, so the teardown
   * thread cannot receive from the side its peer was sending into. Nothing is
   * needed, because the only case that uses a channel sends exactly as many
   * messages as its peer receives and treats every other outcome as fatal, so
   * the channel is always empty by the time it is destroyed. A case that
   * changes that has to drain from the peer's own thread, before it exits. */
  if (st->cq) {
    while (ccol_circq_try_recv_zc(st->cq, &out) == ccol_success) {
    }
  }
  if (st->dq) {
    while (ccol_dynmq_try_recv_zc(st->dq, &out) == ccol_success) {
    }
  }
}

static void q_teardown(void *state) {
  q_state_t *st = state;
  q_drain(st);
  if (st->cq) {
    ccol_circular_queue *q = st->cq;
    ccol_circular_queue_destroy(q);
  }
  if (st->dq) {
    ccol_dynamic_queue *q = st->dq;
    ccol_dynamic_queue_destroy(q);
  }
  if (st->ch) {
    ccol_channel *c = st->ch;
    ccol_channel_destroy(c);
  }
  free(st->payload);
  free(st);
}

static q_state_t *q_alloc(void) {
  q_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->payload_size = BENCH_MSG_SIZE;
  st->payload = calloc(1, st->payload_size);
  if (!st->payload) {
    free(st);
    return NULL;
  }
  return st;
}

static void *circq_setup(size_t n) {
  (void)n;
  q_state_t *st = q_alloc();
  if (!st) return NULL;
  st->cq = ccol_circular_queue_create(BENCH_QUEUE_CAPACITY, NULL);
  if (!st->cq) {
    q_teardown(st);
    return NULL;
  }
  return st;
}

static void *dynq_setup(size_t n) {
  (void)n;
  q_state_t *st = q_alloc();
  if (!st) return NULL;
  st->dq = ccol_dynamic_queue_create(NULL);
  if (!st->dq) {
    q_teardown(st);
    return NULL;
  }
  return st;
}

static void *chan_setup(size_t n) {
  (void)n;
  q_state_t *st = q_alloc();
  if (!st) return NULL;
  st->ch = ccol_channel_create(BENCH_QUEUE_CAPACITY, NULL);
  if (!st->ch) {
    q_teardown(st);
    return NULL;
  }
  return st;
}

static void circq_roundtrip_run(void *state, size_t n) {
  q_state_t *st = state;
  c_message_t msg = {.data = st->payload, .size = st->payload_size};
  for (size_t i = 0; i < n; i++) {
    c_message_t out = {0};
    if (ccol_circq_send_zc(st->cq, &msg) != ccol_success)
      bench_die("circular queue send failed");
    if (ccol_circq_recv_zc(st->cq, &out) != ccol_success)
      bench_die("circular queue receive failed");
    msg = out;
  }
  /* The payload comes back out of the last receive, so the state's own pointer
   * is refreshed to whatever is currently owned; teardown frees exactly one
   * buffer either way. */
  st->payload = msg.data;
  bench_sink(st->payload);
}

static void dynq_roundtrip_run(void *state, size_t n) {
  q_state_t *st = state;
  c_message_t msg = {.data = st->payload, .size = st->payload_size};
  for (size_t i = 0; i < n; i++) {
    c_message_t out = {0};
    if (ccol_dynmq_send_zc(st->dq, &msg) != ccol_success)
      bench_die("dynamic queue send failed");
    if (ccol_dynmq_recv_zc(st->dq, &out) != ccol_success)
      bench_die("dynamic queue receive failed");
    msg = out;
  }
  st->payload = msg.data;
  bench_sink(st->payload);
}

/* A channel is bidirectional and routes by thread identity: the thread that
 * created it sends into the other side's direction and receives from it, so
 * one thread cannot send and then receive its own message. This case therefore
 * always needs a second thread, and is a producer and consumer pair by
 * construction rather than by choice. The creating thread here is the one that
 * ran setup, which is the same thread that runs this function. */
typedef struct {
  ccol_channel *ch;
  size_t n;
} chan_arg_t;

static void *chan_peer_consumer(void *arg) {
  chan_arg_t *a = arg;
  for (size_t i = 0; i < a->n; i++) {
    c_message_t out = {0};
    if (ccol_chan_recv_zc(a->ch, &out) != ccol_success)
      bench_die("channel receive failed");
    bench_sink(out.data);
  }
  return NULL;
}

static void chan_pc_run(void *state, size_t n) {
  q_state_t *st = state;
  chan_arg_t arg = {.ch = st->ch, .n = n};
  pthread_t peer;
  /* Fatal rather than an early return: returning hands the harness the time
   * this function took with no work done in it, which it records as a genuine
   * sample and reports as a spectacular figure. Past this point the send loop
   * must not stop short either, since the peer waits for exactly n messages and
   * a producer that gave up early would leave the join below waiting forever.
   */
  if (pthread_create(&peer, NULL, chan_peer_consumer, &arg) != 0)
    bench_die("channel peer thread could not be started");
  for (size_t i = 0; i < n; i++) {
    c_message_t msg = {.data = st->payload, .size = st->payload_size};
    if (ccol_chan_send_zc(st->ch, &msg) != ccol_success)
      bench_die("channel send failed");
  }
  pthread_join(peer, NULL);
}

/* --- producer and consumer, genuinely contending ------------------------- */

typedef struct {
  ccol_circular_queue *cq;
  size_t n;
  void *seed_payload;
} pc_arg_t;

/* Neither side may quietly stop short. A producer that breaks out early leaves
 * the consumer blocked on a receive that will never be satisfied, and a
 * consumer that breaks out early leaves the producer blocked on a queue that
 * will never drain; in both cases the join below waits forever. Since the only
 * bounded receive and send this module offers compute an absolute deadline per
 * call, using them in the steady state would put a clock read inside the very
 * loop being measured. So the steady state keeps the fast blocking calls and
 * every unexpected outcome aborts instead of breaking.
 *
 * ccol_not_permitted is the one exception: it is how a deliberate
 * ccol_circq_disable_sending reaches a blocked producer, which is exactly the
 * mechanism circq_pc_run uses to unwind when the consumer never started. */
static void *pc_producer(void *arg) {
  pc_arg_t *a = arg;
  /* Every message carries the same buffer, and the consumer frees nothing, so
   * no allocation happens inside the measured loop. A fresh allocation per
   * message would measure the allocator rather than the queue. */
  for (size_t i = 0; i < a->n; i++) {
    c_message_t msg = {.data = a->seed_payload, .size = BENCH_MSG_SIZE};
    ccol_retval_t r = ccol_circq_send_zc(a->cq, &msg);
    if (r == ccol_not_permitted) break;
    if (r != ccol_success) bench_die("circular queue send failed");
  }
  return NULL;
}

static void *pc_consumer(void *arg) {
  pc_arg_t *a = arg;
  for (size_t i = 0; i < a->n; i++) {
    c_message_t out = {0};
    if (ccol_circq_recv_zc(a->cq, &out) != ccol_success)
      bench_die("circular queue receive failed");
    bench_sink(out.data);
  }
  return NULL;
}

static void circq_pc_run(void *state, size_t n) {
  q_state_t *st = state;
  pc_arg_t arg = {.cq = st->cq, .n = n, .seed_payload = st->payload};
  pthread_t prod, cons;

  /* Creating the producer first is what makes the failure path recoverable. If
   * the producer itself cannot start, nothing is running and there is nothing
   * to unwind. If the consumer cannot start, the producer is already running
   * and will block the moment it fills the queue, so it has to be woken
   * deliberately: disable_sending wakes blocked senders (and only senders,
   * which its own documentation is explicit about), and the producer treats
   * the resulting ccol_not_permitted as a clean stop. Joining without that
   * wake-up is an unbounded wait, not a slow one. */
  if (pthread_create(&prod, NULL, pc_producer, &arg) != 0)
    bench_die("producer thread could not be started");
  if (pthread_create(&cons, NULL, pc_consumer, &arg) != 0) {
    ccol_circq_disable_sending(st->cq);
    pthread_join(prod, NULL);
    /* Unwound first, then fatal: the producer has to be woken and joined
     * before anything else happens, and reporting the time this took as a
     * measurement would be worse than stopping. */
    bench_die("consumer thread could not be started");
  }
  pthread_join(prod, NULL);
  pthread_join(cons, NULL);
}

/* ------------------------------------------------------------------------ */
/* GLib comparisons                                                          */
/* ------------------------------------------------------------------------ */

#ifdef BENCH_HAVE_GLIB

typedef struct {
  GAsyncQueue *q;
  GThreadPool *tp;
  gint counter;
  void *payload;
} glib_conc_state_t;

static void glib_conc_teardown(void *state) {
  glib_conc_state_t *st = state;
  if (st->tp) g_thread_pool_free(st->tp, FALSE, TRUE);
  if (st->q) g_async_queue_unref(st->q);
  free(st->payload);
  free(st);
}

static void *glib_asyncq_setup(size_t n) {
  (void)n;
  glib_conc_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->payload = calloc(1, BENCH_MSG_SIZE);
  st->q = g_async_queue_new();
  if (!st->payload || !st->q) {
    glib_conc_teardown(st);
    return NULL;
  }
  return st;
}

BENCH_MT_SETUP(glib_asyncq_setup)

static void glib_asyncq_roundtrip_run(void *state, size_t n) {
  glib_conc_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    g_async_queue_push(st->q, st->payload);
    bench_sink(g_async_queue_pop(st->q));
  }
}

static void glib_pool_task(gpointer data, gpointer user) {
  (void)data;
  glib_conc_state_t *st = user;
  g_atomic_int_inc(&st->counter);
}

static void *glib_pool_setup(size_t n) {
  (void)n;
  glib_conc_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  /* Exclusive, so this pool owns four dedicated worker threads, which is what
     the pool it is compared against is created with. A non-exclusive pool
     dispatches through GLib's shared global pool instead, so the two would not
     be running the same number of workers on the same work. */
  st->tp = g_thread_pool_new(glib_pool_task, st, 4, TRUE, NULL);
  if (!st->tp) {
    free(st);
    return NULL;
  }
  return st;
}

/* Drains by waiting for every task to have run, which is what ctpool_wait does
 * on the other side of this comparison, and leaves the pool standing.
 * g_thread_pool_free(wait=TRUE) would also drain, but it returns and joins the
 * worker threads as well, so the arm would be timed doing a teardown the arm it
 * is compared against does not do. */
static void glib_pool_submit_run(void *state, size_t n) {
  glib_conc_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    g_thread_pool_push(st->tp, GINT_TO_POINTER(1), NULL);
  /* Parked on a short sleep rather than a spin, for the reason this project's
     own drain loops give: under an instrumented run one thread executes at a
     time, and a yield-spin burns whole quanta the worker it waits for needs. */
  /* Bounded. A push that was silently dropped would otherwise leave this
     spinning for the life of the process rather than failing, which is the
     shape of hang this project's own lessons single out: a wait on a flag only
     another thread can set needs a ceiling. Ten seconds is far past any honest
     completion of this workload and far short of a CI timeout. */
  double deadline = bench_now_ns() + 10e9;
  while ((size_t)g_atomic_int_get(&st->counter) < n) {
    if (bench_now_ns() > deadline)
      bench_die("GThreadPool drain did not complete");
    struct timespec ts = {0, 50000};
    nanosleep(&ts, NULL);
  }
  bench_sink(&st->counter);
}

#endif /* BENCH_HAVE_GLIB */

/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* Multi-threaded variants                                                   */
/* ------------------------------------------------------------------------ */

/* One pool, several submitting threads, and as many workers as submitters.
 * Scaling both together is what asks whether the pool as a whole scales: a
 * fixed worker count would cap throughput no matter how the submission path
 * behaved, and the figure would describe the cap rather than the pool. */
static void *pool_setup_workers_mt(size_t n, unsigned threads) {
  (void)n;
  return pool_setup_n(threads);
}

/* The shared-pool variant submits and does not drain, which is why it is named
 * for submission alone rather than sharing the single-threaded cases' name.
 * ctpool_wait documents that no other thread may be submitting while it runs,
 * and on an unbounded queue a concurrent submitter can block it indefinitely;
 * every thread here is submitting into the one pool, so calling it would both
 * break that contract and measure the wrong thing, since the first thread to
 * finish submitting would be timed waiting out every other thread's work. What
 * this case reports is the cost of getting a task into a pool several threads
 * are feeding at once. The queue is drained by teardown, outside the clock. */
static void pool_submit_only_run(void *state, size_t n) {
  pool_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    /* See pool_submit_run. */
    if (ctpool_submit(st->pool, pool_task, st, NULL) != ccol_success)
      bench_die("ctpool_submit failed");
  }
  bench_sink(&st->counter);
}

/* The queues are internally locked, so their threads share one queue. Each
 * thread owns one payload buffer and has at most one message outstanding, so
 * the queue never holds more than one message per thread and a receiver that
 * is waiting always has a sender ahead of it.
 *
 * The buffers move between threads: a zero-copy send hands ownership to the
 * queue and a receive takes ownership of whatever comes out, which need not be
 * what this thread put in. The set of buffers is conserved, so each thread
 * parks whatever it ends up holding in its own slot and teardown frees all of
 * them. */
typedef struct {
  q_state_t *q;
  void **payloads;
  size_t payload_count;
} q_mt_state_t;

static void q_mt_teardown(void *state) {
  q_mt_state_t *st = state;
  for (size_t i = 0; i < st->payload_count; i++) free(st->payloads[i]);
  free(st->payloads);
  if (st->q) {
    /* The shared q_state_t's own payload is already accounted for above. */
    st->q->payload = NULL;
    q_teardown(st->q);
  }
  free(st);
}

static void *q_mt_setup(void *(*inner)(size_t), unsigned threads) {
  q_mt_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->payloads = calloc(threads, sizeof(void *));
  if (!st->payloads) {
    free(st);
    return NULL;
  }
  st->payload_count = threads;
  for (unsigned i = 0; i < threads; i++) {
    st->payloads[i] = calloc(1, BENCH_MSG_SIZE);
    if (!st->payloads[i]) {
      q_mt_teardown(st);
      return NULL;
    }
  }
  st->q = inner(0);
  if (!st->q) {
    q_mt_teardown(st);
    return NULL;
  }
  /* q_alloc gave the inner state a payload of its own that nothing sends. */
  free(st->q->payload);
  st->q->payload = NULL;
  return st;
}

static void *circq_mt_setup(size_t n, unsigned threads) {
  (void)n;
  return q_mt_setup(circq_setup, threads);
}

static void *dynq_mt_setup(size_t n, unsigned threads) {
  (void)n;
  return q_mt_setup(dynq_setup, threads);
}

static void circq_roundtrip_mt_run(void *state, size_t n) {
  q_mt_state_t *st = state;
  void **mine = &st->payloads[bench_thread_index()];
  c_message_t msg = {.data = *mine, .size = BENCH_MSG_SIZE};
  for (size_t i = 0; i < n; i++) {
    c_message_t out = {0};
    if (ccol_circq_send_zc(st->q->cq, &msg) != ccol_success)
      bench_die("circular queue send failed");
    if (ccol_circq_recv_zc(st->q->cq, &out) != ccol_success)
      bench_die("circular queue receive failed");
    msg = out;
  }
  *mine = msg.data;
  bench_sink(*mine);
}

static void dynq_roundtrip_mt_run(void *state, size_t n) {
  q_mt_state_t *st = state;
  void **mine = &st->payloads[bench_thread_index()];
  c_message_t msg = {.data = *mine, .size = BENCH_MSG_SIZE};
  for (size_t i = 0; i < n; i++) {
    c_message_t out = {0};
    if (ccol_dynmq_send_zc(st->q->dq, &msg) != ccol_success)
      bench_die("dynamic queue send failed");
    if (ccol_dynmq_recv_zc(st->q->dq, &out) != ccol_success)
      bench_die("dynamic queue receive failed");
    msg = out;
  }
  *mine = msg.data;
  bench_sink(*mine);
}

void bench_register_concurrency(void) {
  bench_add(&(bench_case_t){.group = "cthreadpool",
                            .name = "submit_and_drain_1_worker",
                            .setup = pool_setup_1,
                            .run = pool_submit_run,
                            .teardown = pool_teardown,
                            .n = BENCH_POOL_N});
  bench_add(&(bench_case_t){.group = "cthreadpool",
                            .name = "submit_and_drain_4_workers",
                            .setup = pool_setup_4,
                            .run = pool_submit_run,
                            .teardown = pool_teardown,
                            .n = BENCH_POOL_N});
  bench_add_mt(&(bench_case_t){.group = "cthreadpool",
                               .name = "submit_contended",
                               .setup_mt = pool_setup_workers_mt,
                               .run = pool_submit_only_run,
                               .teardown = pool_teardown,
                               .n = BENCH_POOL_N,
                               .shared_fixture = true});
#ifdef BENCH_HAVE_GLIB
  bench_add(&(bench_case_t){.group = "cthreadpool",
                            .name = "submit_and_drain_4_workers",
                            .vs = "GThreadPool",
                            .setup = glib_pool_setup,
                            .run = glib_pool_submit_run,
                            .teardown = glib_conc_teardown,
                            .n = BENCH_POOL_N});
#endif

  bench_add(&(bench_case_t){.group = "cthreadcomm",
                            .name = "circular_queue_roundtrip",
                            .setup = circq_setup,
                            .run = circq_roundtrip_run,
                            .teardown = q_teardown,
                            .n = BENCH_QUEUE_N});
  bench_add_mt(&(bench_case_t){.group = "cthreadcomm",
                               .name = "circular_queue_roundtrip",
                               .setup_mt = circq_mt_setup,
                               .run = circq_roundtrip_mt_run,
                               .teardown = q_mt_teardown,
                               .n = BENCH_QUEUE_MT_N,
                               .shared_fixture = true});
  bench_add(&(bench_case_t){.group = "cthreadcomm",
                            .name = "dynamic_queue_roundtrip",
                            .setup = dynq_setup,
                            .run = dynq_roundtrip_run,
                            .teardown = q_teardown,
                            .n = BENCH_QUEUE_N});
  bench_add_mt(&(bench_case_t){.group = "cthreadcomm",
                               .name = "dynamic_queue_roundtrip",
                               .setup_mt = dynq_mt_setup,
                               .run = dynq_roundtrip_mt_run,
                               .teardown = q_mt_teardown,
                               .n = BENCH_QUEUE_MT_N,
                               .shared_fixture = true});
  bench_add(&(bench_case_t){.group = "cthreadcomm",
                            .name = "channel_producer_consumer",
                            .setup = chan_setup,
                            .run = chan_pc_run,
                            .teardown = q_teardown,
                            .n = BENCH_QUEUE_N});
  bench_add(&(bench_case_t){.group = "cthreadcomm",
                            .name = "circular_queue_producer_consumer",
                            .setup = circq_setup,
                            .run = circq_pc_run,
                            .teardown = q_teardown,
                            .n = BENCH_QUEUE_N});
#ifdef BENCH_HAVE_GLIB
  bench_add(&(bench_case_t){.group = "cthreadcomm",
                            .name = "circular_queue_roundtrip",
                            .vs = "GAsyncQueue",
                            .setup = glib_asyncq_setup,
                            .run = glib_asyncq_roundtrip_run,
                            .teardown = glib_conc_teardown,
                            .n = BENCH_QUEUE_N});
  /* The same shape against GLib's own queue: one queue, every thread pushing
   * and popping. It answers whether a shared blocking queue behaves this way
   * for any implementation or only for this one, which is not something the
   * single-threaded comparison can settle. */
  bench_add_mt(&(bench_case_t){.group = "cthreadcomm",
                               .name = "circular_queue_roundtrip",
                               .vs = "GAsyncQueue",
                               .setup_mt = glib_asyncq_setup_mt,
                               .run = glib_asyncq_roundtrip_run,
                               .teardown = glib_conc_teardown,
                               .n = BENCH_QUEUE_MT_N,
                               .shared_fixture = true});
#endif
}
